// Concord
//
// Copyright (c) 2018-2021 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").
// You may not use this product except in compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#include "reconfiguration_kvbc_handler.hpp"
#include "ControlStateManager.hpp"
#include "bftengine/EpochManager.hpp"
#include "bftengine/ReconfigurationCmd.hpp"
#include "bftengine/DbCheckpointManager.hpp"
#include "bftengine/SigManager.hpp"
#include "util/endianness.hpp"
#include "kvbc_app_filter/kvbc_app_filter.h"
#include "kvbc_app_filter/kvbc_key_types.h"
#include "concord.cmf.hpp"
#include "secrets/secrets_manager_plain.h"
#include "rocksdb/native_client.h"
#include "kvbc_adapter/idempotent_reader.h"
#include "categorization/db_categories.h"
#include "categorization/details.h"
#include "categorized_kvbc_msgs.cmf.hpp"
#include "metadata_block_id.h"
#include "crypto/crypto.hpp"

#include <chrono>
#include <algorithm>
#include <memory>

namespace concord::kvbc::reconfiguration {
using bftEngine::ReplicaConfig;
using concord::crypto::SignatureAlgorithm;
using bftEngine::impl::DbCheckpointManager;
using bftEngine::impl::SigManager;
using concord::kvbc::KvbAppFilter;
using concord::kvbc::adapter::IdempotentReader;
using concord::messages::SnapshotResponseStatus;
using concord::storage::rocksdb::NativeClient;
using concord::crypto::EdDSAHexToPem;

std::optional<categorization::Value> get(const std::string& key, BlockId id, kvbc::IReader& ro_storage) {
  auto opt_val = ro_storage.getLatest(concord::kvbc::categorization::kConcordReconfigurationCategoryId, key);
  if (!opt_val || std::get<categorization::VersionedValue>(*opt_val).block_id != id) {
    LOG_INFO(GL, "Need to call explicit get");
    return ro_storage.get(concord::kvbc::categorization::kConcordReconfigurationCategoryId, key, id);
  }
  LOG_DEBUG(GL, "Get Latest found the correct version");
  return opt_val;
}

kvbc::BlockId ReconfigurationBlockTools::persistReconfigurationBlock(
    const std::vector<uint8_t>& data,
    const uint64_t bft_seq_num,
    std::string key,
    const std::optional<bftEngine::Timestamp>& timestamp,
    bool include_wedge) {
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  uint64_t epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  auto current_epoch_buf = concordUtils::toBigEndianStringBuffer(epoch);
  // Set the global epoch number
  ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key}, concordUtils::toBigEndianStringBuffer(epoch));
  // Set the epoch number of this action
  ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key} + key,
                        concordUtils::toBigEndianStringBuffer(epoch));
  ver_updates.addUpdate(std::move(key), std::string(data.begin(), data.end()));
  try {
    return persistReconfigurationBlock(ver_updates, bft_seq_num, timestamp, include_wedge);
  } catch (const std::exception& e) {
    LOG_ERROR(GL, "failed to persist the reconfiguration block: " << e.what());
    throw;
  }
}

kvbc::BlockId ReconfigurationBlockTools::persistReconfigurationBlock(
    concord::kvbc::categorization::VersionedUpdates& ver_updates,
    const uint64_t bft_seq_num,
    const std::optional<bftEngine::Timestamp>& timestamp,
    bool include_wedge) {
  // All blocks are expected to have the BFT sequence number as a key.
  if (timestamp.has_value()) {
    ver_updates.addUpdate(std::string{keyTypes::reconfiguration_ts_key},
                          concordUtils::toBigEndianStringBuffer(timestamp.value().time_since_epoch.count()));
  }
  if (include_wedge) {
    concord::messages::WedgeCommand wedge_command;
    std::vector<uint8_t> wedge_buf;
    concord::messages::serialize(wedge_buf, wedge_command);
    ver_updates.addUpdate(std::string{keyTypes::reconfiguration_wedge_key},
                          std::string(wedge_buf.begin(), wedge_buf.end()));
  }
  concord::kvbc::categorization::Updates updates;
  updates.add(concord::kvbc::categorization::kConcordReconfigurationCategoryId, std::move(ver_updates));
  concord::kvbc::categorization::VersionedUpdates sn_updates;
  sn_updates.addUpdate(std::string{kvbc::keyTypes::bft_seq_num_key}, block_metadata_.serialize(bft_seq_num));
  updates.add(concord::kvbc::categorization::kConcordInternalCategoryId, std::move(sn_updates));

  try {
    auto ret = blocks_adder_.add(std::move(updates));
    LOG_INFO(GL, "Persist result: " << KVLOG(ret));
    return ret;
  } catch (const std::exception& e) {
    LOG_ERROR(GL, "failed to persist the reconfiguration block: " << e.what());
    throw;
  }
}

kvbc::BlockId ReconfigurationBlockTools::persistNewEpochBlock(const uint64_t bft_seq_num) {
  auto newEpoch = bftEngine::EpochManager::instance().getSelfEpochNumber() + 1;
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  ver_updates.addUpdate(std::string{kvbc::keyTypes::reconfiguration_epoch_key},
                        concordUtils::toBigEndianStringBuffer(newEpoch));
  auto block_id = persistReconfigurationBlock(ver_updates, bft_seq_num, std::nullopt, false);
  bftEngine::EpochManager::instance().setSelfEpochNumber(newEpoch);
  bftEngine::EpochManager::instance().setGlobalEpochNumber(newEpoch);
  LOG_INFO(GL, "Starting new epoch " << KVLOG(newEpoch, block_id));
  return block_id;
}
concord::messages::ClientStateReply KvbcClientReconfigurationHandler::buildClientStateReply(
    kvbc::keyTypes::CLIENT_COMMAND_TYPES command_type, uint32_t clientid) {
  concord::messages::ClientStateReply creply;
  creply.block_id = 0;
  auto res = ro_storage_.getLatest(
      concord::kvbc::categorization::kConcordReconfigurationCategoryId,
      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix, static_cast<char>(command_type)} +
          std::to_string(clientid));
  if (res.has_value()) {
    std::visit(
        [&](auto&& arg) {
          auto strval = arg.data;
          std::vector<uint8_t> data_buf(strval.begin(), strval.end());
          switch (command_type) {
            case kvbc::keyTypes::CLIENT_COMMAND_TYPES::PUBLIC_KEY_EXCHANGE: {
              concord::messages::ClientExchangePublicKey cmd;
              concord::messages::deserialize(data_buf, cmd);
              creply.response = cmd;
              break;
            }
            case kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_KEY_EXCHANGE_COMMAND: {
              concord::messages::ClientKeyExchangeCommand cmd;
              concord::messages::deserialize(data_buf, cmd);
              creply.response = cmd;
              break;
            }
            case kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_EXECUTE_COMMAND: {
              concord::messages::ClientsAddRemoveExecuteCommand cmd;
              concord::messages::deserialize(data_buf, cmd);
              creply.response = cmd;
              break;
            }
            case kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_COMMAND_STATUS: {
              concord::messages::ClientsAddRemoveUpdateCommand cmd;
              concord::messages::deserialize(data_buf, cmd);
              creply.response = cmd;
              break;
            }
            case kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_RESTART_COMMAND: {
              concord::messages::ClientsRestartCommand cmd;
              concord::messages::deserialize(data_buf, cmd);
              creply.response = cmd;
              break;
            }
            default:
              break;
          }
          creply.block_id = arg.block_id;
          auto epoch_data =
              get(std::string{kvbc::keyTypes::reconfiguration_epoch_key} +
                      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix, static_cast<char>(command_type)} +
                      std::to_string(clientid),
                  arg.block_id,
                  ro_storage_);
          ConcordAssert(epoch_data.has_value());
          const auto& epoch_str = std::get<categorization::VersionedValue>(*epoch_data).data;
          ConcordAssertEQ(epoch_str.size(), sizeof(uint64_t));
          uint64_t epoch = concordUtils::fromBigEndianBuffer<uint64_t>(epoch_str.data());
          creply.epoch = epoch;
        },
        *res);
  }
  return creply;
}

bool StateSnapshotReconfigurationHandler::handle(const concord::messages::StateSnapshotRequest& cmd,
                                                 uint64_t sequence_number,
                                                 uint32_t,
                                                 const std::optional<bftEngine::Timestamp>& timestamp,
                                                 concord::messages::ReconfigurationResponse& rres) {
  if (!ReplicaConfig::instance().dbCheckpointFeatureEnabled ||
      ReplicaConfig::instance().maxNumberOfDbCheckpoints == 0) {
    const auto err = "StateSnapshotRequest(participant ID = " + cmd.participant_id +
                     "): failed, the DB checkpoint feature is disabled";
    LOG_WARN(getLogger(), err);
    rres.response = concord::messages::ReconfigurationErrorMsg{err};
    return false;
  }

  auto resp = concord::messages::StateSnapshotResponse{};
  const auto last_checkpoint_desc = DbCheckpointManager::instance().getLastCreatedDbCheckpointMetadata();
  if (last_checkpoint_desc) {
    resp.data.emplace();
    resp.data->snapshot_id = last_checkpoint_desc->checkPointId_;
    const auto read_only = true;
    auto db = NativeClient::newClient(
        DbCheckpointManager::instance().getPathForCheckpoint(last_checkpoint_desc->checkPointId_),
        read_only,
        NativeClient::DefaultOptions{});
    const auto link_st_chain = false;
    const auto idempotent_kvbc = std::make_shared<const adapter::ReplicaBlockchain>(db, link_st_chain);
    const auto reader = IdempotentReader{idempotent_kvbc};
    const auto filter = KvbAppFilter{&reader, ""};
    if (ReplicaConfig::instance().enableEventGroups) {
      // TODO: We currently only support new participants and, therefore, the event group ID will always be the last
      // (newest) public event group ID.
      resp.data->blockchain_height = filter.getNewestPublicEventGroupId();
      resp.data->blockchain_height_type = messages::BlockchainHeightType::EventGroupId;
    } else {
      resp.data->blockchain_height = reader.getLastBlockId();
      resp.data->blockchain_height_type = messages::BlockchainHeightType::BlockId;
    }
    const auto public_state = idempotent_kvbc->getPublicStateKeys();
    if (!public_state) {
      resp.data->key_value_count_estimate = 0;
    } else {
      resp.data->key_value_count_estimate = public_state->keys.size();
    }
    resp.data->last_application_transaction_time = last_app_txn_time_cb_(reader);
    LOG_INFO(getLogger(),
             "StateSnapshotRequest(participant ID = " << cmd.participant_id << "): using existing last checkpoint ID: "
                                                      << last_checkpoint_desc->checkPointId_);
  } else {
    const auto checkpoint_id =
        DbCheckpointManager::instance().createDbCheckpointAsync(sequence_number, timestamp, std::nullopt);
    if (checkpoint_id) {
      resp.data.emplace();
      resp.data->snapshot_id = *checkpoint_id;
      const auto filter = KvbAppFilter{&ro_storage_, ""};
      if (ReplicaConfig::instance().enableEventGroups) {
        // TODO: We currently only support new participants and, therefore, the event group ID will always be the last
        // (newest) public event group ID.
        resp.data->blockchain_height = filter.getNewestPublicEventGroupId();
        resp.data->blockchain_height_type = messages::BlockchainHeightType::EventGroupId;
      } else {
        resp.data->blockchain_height = ro_storage_.getLastBlockId();
        resp.data->blockchain_height_type = messages::BlockchainHeightType::BlockId;
      }
      // If we are creating the snapshot now, return an estimate based on the blockchain and not on the snapshot itself
      // (as it is created asynchronously).
      const auto opt_val =
          ro_storage_.getLatest(categorization::kConcordInternalCategoryId, keyTypes::state_public_key_set);
      if (!opt_val) {
        resp.data->key_value_count_estimate = 0;
      } else {
        auto public_state = categorization::PublicStateKeys{};
        const auto val = std::get_if<categorization::VersionedValue>(&opt_val.value());
        ConcordAssertNE(val, nullptr);
        categorization::detail::deserialize(val->data, public_state);
        resp.data->key_value_count_estimate = public_state.keys.size();
        resp.data->last_application_transaction_time = last_app_txn_time_cb_(ro_storage_);
      }
      LOG_INFO(getLogger(),
               "StateSnapshotRequest(participant ID = " << cmd.participant_id
                                                        << "): creating checkpoint with ID: " << *checkpoint_id);
    } else {
      // If we couldn't create a DB checkpoint and there is no last one created, we just leave `resp.data`
      // nullopt, indicating to the client that it should retry.
      LOG_INFO(getLogger(),
               "StateSnapshotRequest(participant ID = "
                   << cmd.participant_id
                   << "): cannot create a checkpoint and there is no existing one, client must retry");
    }
  }
  rres.response = std::move(resp);
  return true;
}

bool StateSnapshotReconfigurationHandler::handle(const concord::messages::SignedPublicStateHashRequest& req,
                                                 uint64_t,
                                                 uint32_t,
                                                 const std::optional<bftEngine::Timestamp>&,
                                                 concord::messages::ReconfigurationResponse& reconf_resp) {
  using concord::kvbc::categorization::StateHash;
  using concord::kvbc::categorization::detail::deserialize;
  using concord::kvbc::categorization::detail::serialize;
  using concord::messages::SignedPublicStateHashResponse;

  auto resp = SignedPublicStateHashResponse{};
  const auto state = DbCheckpointManager::instance().getCheckpointState(req.snapshot_id);
  switch (state) {
    case DbCheckpointManager::CheckpointState::kNonExistent:
      LOG_INFO(getLogger(),
               "SignedPublicStateHashRequest: snapshot ID = "
                   << req.snapshot_id << " is non-existent, requesting participant ID = " << req.participant_id);
      resp.status = SnapshotResponseStatus::SnapshotNonExistent;
      break;
    case DbCheckpointManager::CheckpointState::kPending:
      LOG_INFO(getLogger(),
               "SignedPublicStateHashRequest: snapshot ID = "
                   << req.snapshot_id << " is pending creation, requesting participant ID = " << req.participant_id);
      resp.status = SnapshotResponseStatus::SnapshotPending;
      break;
    case DbCheckpointManager::CheckpointState::kCreated: {
      const auto snapshot_path = DbCheckpointManager::instance().getPathForCheckpoint(req.snapshot_id);
      const auto read_only = true;
      try {
        auto db = NativeClient::newClient(snapshot_path, read_only, NativeClient::DefaultOptions{});
        const auto ser_hash = db->get(concord::kvbc::bcutil::BlockChainUtils::publicStateHashKey());
        if (!ser_hash) {
          LOG_ERROR(getLogger(),
                    "SignedPublicStateHashRequest: missing public state hash for snapshot ID = "
                        << req.snapshot_id << ", requesting participant ID = " << req.participant_id);
          resp.status = SnapshotResponseStatus::InternalError;
        } else {
          auto public_state_hash = StateHash{};
          deserialize(*ser_hash, public_state_hash);
          resp.status = SnapshotResponseStatus::Success;
          resp.data.snapshot_id = req.snapshot_id;
          resp.data.replica_id = ReplicaConfig::instance().replicaId;
          resp.data.block_id = public_state_hash.block_id;
          resp.data.hash = public_state_hash.hash;
          resp.signature.assign(SigManager::instance()->getMySigLength(), 0);
          const auto data_ser = serialize(resp.data);
          SigManager::instance()->sign(reinterpret_cast<const char*>(data_ser.data()),
                                       data_ser.size(),
                                       reinterpret_cast<char*>(resp.signature.data()));
          LOG_INFO(getLogger(),
                   "SignedPublicStateHashRequest: successful request for snapshot ID = "
                       << req.snapshot_id << ", requesting participant ID = " << req.participant_id);
        }
      } catch (const std::exception& e) {
        LOG_ERROR(getLogger(),
                  "SignedPublicStateHashRequest: failed for snapshot ID = "
                      << req.snapshot_id << ", requesting participant ID = " << req.participant_id
                      << ", error =  " << e.what());
        resp.status = SnapshotResponseStatus::InternalError;
      }
    } break;
  }
  reconf_resp.response = std::move(resp);
  return true;
}

bool StateSnapshotReconfigurationHandler::handle(const concord::messages::StateSnapshotReadAsOfRequest& req,
                                                 uint64_t,
                                                 uint32_t,
                                                 const std::optional<bftEngine::Timestamp>&,
                                                 concord::messages::ReconfigurationResponse& reconf_resp) {
  auto resp = concord::messages::StateSnapshotReadAsOfResponse{};
  const auto state = DbCheckpointManager::instance().getCheckpointState(req.snapshot_id);
  switch (state) {
    case DbCheckpointManager::CheckpointState::kNonExistent:
      LOG_INFO(getLogger(),
               "StateSnapshotReadAsOfResponse: snapshot ID = "
                   << req.snapshot_id << " is non-existent, requesting participant ID = " << req.participant_id);
      resp.status = SnapshotResponseStatus::SnapshotNonExistent;
      break;
    case DbCheckpointManager::CheckpointState::kPending:
      LOG_INFO(getLogger(),
               "StateSnapshotReadAsOfResponse: snapshot ID = "
                   << req.snapshot_id << " is pending creation, requesting participant ID = " << req.participant_id);
      resp.status = SnapshotResponseStatus::SnapshotPending;
      break;
    case DbCheckpointManager::CheckpointState::kCreated: {
      const auto snapshot_path = DbCheckpointManager::instance().getPathForCheckpoint(req.snapshot_id);
      const auto read_only = true;
      try {
        auto db = NativeClient::newClient(snapshot_path, read_only, NativeClient::DefaultOptions{});
        const auto link_st_chain = false;
        const auto kvbc = adapter::ReplicaBlockchain{db, link_st_chain};
        const auto public_state = kvbc.getPublicStateKeys();
        auto values = std::vector<std::optional<categorization::Value>>{};
        kvbc.multiGetLatest(categorization::kExecutionProvableCategory, req.keys, values);
        ConcordAssertEQ(req.keys.size(), values.size());
        for (auto i = 0ull; i < req.keys.size(); ++i) {
          auto& val = values[i];
          const auto& key = req.keys[i];
          if (!val) {
            resp.values.push_back(std::nullopt);
          } else {
            auto merkle_val = std::get_if<categorization::MerkleValue>(&val.value());
            ConcordAssertNE(merkle_val, nullptr);
            // Make sure no non-public keys are requested.
            // TODO: This will change when we start streaming non-public keys.
            if (public_state) {
              auto it = std::lower_bound(public_state->keys.cbegin(), public_state->keys.cend(), key);
              if (it == public_state->keys.cend() || *it != key) {
                resp.values.push_back(std::nullopt);
              } else {
                resp.values.push_back(state_value_converter_(std::move(merkle_val->data)));
              }
            } else {
              resp.values.push_back(std::nullopt);
            }
          }
        }
        resp.status = SnapshotResponseStatus::Success;
        LOG_DEBUG(getLogger(),
                  "StateSnapshotReadAsOfResponse: successful request for snapshot ID = "
                      << req.snapshot_id << ", requesting participant ID = " << req.participant_id);
      } catch (const std::exception& e) {
        LOG_ERROR(getLogger(),
                  "StateSnapshotReadAsOfResponse: failed for snapshot ID = "
                      << req.snapshot_id << ", requesting participant ID = " << req.participant_id
                      << ", error =  " << e.what());
        resp.status = SnapshotResponseStatus::InternalError;
      }
      break;
    }
  }
  reconf_resp.response = std::move(resp);
  return true;
}

concord::messages::ClientStateReply KvbcClientReconfigurationHandler::buildReplicaStateReply(
    const std::string& command_type, uint32_t clientid) {
  concord::messages::ClientStateReply creply;
  creply.block_id = 0;
  auto res = ro_storage_.getLatest(concord::kvbc::categorization::kConcordReconfigurationCategoryId,
                                   command_type + std::to_string(clientid));
  if (res.has_value()) {
    std::visit(
        [&](auto&& arg) {
          auto strval = arg.data;
          std::vector<uint8_t> data_buf(strval.begin(), strval.end());
          if (command_type == std::string{kvbc::keyTypes::reconfiguration_tls_exchange_key}) {
            concord::messages::ReplicaTlsExchangeKey cmd;
            concord::messages::deserialize(data_buf, cmd);
            creply.response = cmd;
          } else if (command_type ==
                     std::string{
                         kvbc::keyTypes::reconfiguration_client_data_prefix,
                         static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_EXECUTE_COMMAND)}) {
            concord::messages::ClientsAddRemoveExecuteCommand cmd;
            concord::messages::deserialize(data_buf, cmd);
            creply.response = cmd;
          } else if (command_type == std::string{kvbc::keyTypes::reconfiguration_rep_main_key}) {
            concord::messages::ReplicaMainKeyUpdate cmd;
            concord::messages::deserialize(data_buf, cmd);
            creply.response = cmd;
          }
          auto epoch_data =
              get(std::string{kvbc::keyTypes::reconfiguration_epoch_key} + command_type + std::to_string(clientid),
                  arg.block_id,
                  ro_storage_);
          ConcordAssert(epoch_data.has_value());
          const auto& epoch_str = std::get<categorization::VersionedValue>(*epoch_data).data;
          ConcordAssertEQ(epoch_str.size(), sizeof(uint64_t));
          uint64_t epoch = concordUtils::fromBigEndianBuffer<uint64_t>(epoch_str.data());
          creply.epoch = epoch;
          creply.block_id = arg.block_id;
        },
        *res);
  }
  return creply;
}
bool KvbcClientReconfigurationHandler::handle(const concord::messages::ClientReconfigurationStateRequest& command,
                                              uint64_t bft_seq_num,
                                              uint32_t sender_id,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse& rres) {
  concord::messages::ClientReconfigurationStateReply rep;
  uint16_t first_client_id = ReplicaConfig::instance().numReplicas + ReplicaConfig::instance().numRoReplicas;
  if (sender_id > first_client_id) {
    for (uint8_t i = kvbc::keyTypes::CLIENT_COMMAND_TYPES::start_ + 1; i < kvbc::keyTypes::CLIENT_COMMAND_TYPES::end_;
         i++) {
      auto csrep = buildClientStateReply(static_cast<keyTypes::CLIENT_COMMAND_TYPES>(i), sender_id);
      if (csrep.block_id == 0) continue;
      rep.states.push_back(csrep);
    }
    for (uint16_t i = 0; i < first_client_id; i++) {
      auto ke_csrep = buildReplicaStateReply(std::string{kvbc::keyTypes::reconfiguration_rep_main_key}, i);
      if (ke_csrep.block_id > 0) rep.states.push_back(ke_csrep);
    }
  } else {
    auto scaling_key_prefix =
        std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                    static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_EXECUTE_COMMAND)};
    for (uint16_t i = 0; i < first_client_id; i++) {
      if (i == sender_id) continue;
      // 1. Handle TLS key exchange update
      auto ke_csrep = buildReplicaStateReply(std::string{kvbc::keyTypes::reconfiguration_tls_exchange_key}, i);
      if (ke_csrep.block_id > 0) rep.states.push_back(ke_csrep);
      // 2. Handle scaling command
      auto scale_csrep = buildReplicaStateReply(scaling_key_prefix, i);
      if (scale_csrep.block_id > 0) rep.states.push_back(scale_csrep);
      // 3. Handler scaling status update
      auto scale_status_csrep = buildReplicaStateReply(
          std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                      static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_COMMAND_STATUS)},
          i);
      if (scale_status_csrep.block_id > 0) rep.states.push_back(scale_csrep);
    }
  }
  concord::messages::serialize(rres.additional_data, rep);
  return true;
}

// TODO(yf): this is where client key exchange is published
bool KvbcClientReconfigurationHandler::handle(const concord::messages::ClientExchangePublicKey& command,
                                              uint64_t bft_seq_num,
                                              uint32_t sender_id,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command,
      bft_seq_num,
      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                  static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::PUBLIC_KEY_EXCHANGE)} +
          std::to_string(sender_id),
      ts,
      false);
  LOG_INFO(getLogger(), "block id: " << blockId);
  return true;
}

bool KvbcClientReconfigurationHandler::handle(const concord::messages::ClientsAddRemoveUpdateCommand& command,
                                              uint64_t bft_seq_num,
                                              uint32_t sender_id,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command,
      bft_seq_num,
      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                  static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_COMMAND_STATUS)} +
          std::to_string(sender_id),
      ts,
      false);
  LOG_INFO(getLogger(), "block id: " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::ClientsAddRemoveStatusCommand&,
                                    uint64_t,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& rres) {
  concord::messages::ClientsAddRemoveStatusResponse stats;
  for (const auto& gr : ReplicaConfig::instance().clientGroups) {
    for (auto cid : gr.second) {
      std::string key =
          std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                      static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_COMMAND_STATUS)} +
          std::to_string(cid);
      auto res = ro_storage_.getLatest(concord::kvbc::categorization::kConcordReconfigurationCategoryId, key);
      if (res.has_value()) {
        auto strval = std::visit([](auto&& arg) { return arg.data; }, *res);
        concord::messages::ClientsAddRemoveUpdateCommand cmd;
        std::vector<uint8_t> bytesval(strval.begin(), strval.end());
        concord::messages::deserialize(bytesval, cmd);

        LOG_INFO(getLogger(), "found scaling status for client" << KVLOG(cid, cmd.config_descriptor));
        stats.clients_status.push_back(std::make_pair(cid, cmd.config_descriptor));
      }
    }
  }
  rres.response = stats;
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::ClientKeyExchangeStatus& command,
                                    uint64_t,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& rres) {
  concord::messages::ClientKeyExchangeStatusResponse stats;
  concord::secretsmanager::SecretsManagerPlain psm;
  for (const auto& gr : ReplicaConfig::instance().clientGroups) {
    for (auto cid : gr.second) {
      if (command.tls) {
        const std::string base_path = ReplicaConfig::instance().certificatesRootPath + "/" + std::to_string(cid);
        std::string client_cert_path = (ReplicaConfig::instance().useUnifiedCertificates)
                                           ? base_path + "/node.cert"
                                           : base_path + "/client/client.cert";
        auto cert = psm.decryptFile(client_cert_path).value_or("invalid client id");
        stats.clients_data.push_back(std::make_pair(cid, cert));
        continue;
      }
      std::string key = std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                                    static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::PUBLIC_KEY_EXCHANGE)} +
                        std::to_string(cid);
      auto bid = ro_storage_.getLatestVersion(concord::kvbc::categorization::kConcordReconfigurationCategoryId, key);
      if (bid.has_value()) {
        auto saved_ts = get(std::string{kvbc::keyTypes::reconfiguration_ts_key}, bid.value().version, ro_storage_);
        uint64_t numeric_ts{0};
        if (saved_ts.has_value()) {
          auto strval = std::visit([](auto&& arg) { return arg.data; }, *saved_ts);
          numeric_ts = concordUtils::fromBigEndianBuffer<uint64_t>(strval.data());
          stats.timestamps.push_back(std::make_pair(cid, numeric_ts));
        }
        auto res = get(key, bid.value().version, ro_storage_);
        if (res.has_value()) {
          auto strval = std::visit([](auto&& arg) { return arg.data; }, *res);
          concord::messages::ClientExchangePublicKey cmd;
          std::vector<uint8_t> bytesval(strval.begin(), strval.end());
          concord::messages::deserialize(bytesval, cmd);

          LOG_INFO(getLogger(), "found transactions public key exchange status for client" << KVLOG(cid));
          stats.clients_data.push_back(std::make_pair(cid, cmd.pub_key));
        }
      }
    }
  }
  rres.response = stats;
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::WedgeCommand& command,
                                    uint64_t bft_seq_num,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, bft_seq_num, std::string{kvbc::keyTypes::reconfiguration_wedge_key}, ts, false);
  LOG_INFO(getLogger(), "WedgeCommand block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::DownloadCommand& command,
                                    uint64_t bft_seq_num,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, bft_seq_num, std::string{kvbc::keyTypes::reconfiguration_download_key}, ts, false);
  LOG_INFO(getLogger(), "DownloadCommand command block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::InstallCommand& command,
                                    uint64_t bft_seq_num,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, bft_seq_num, std::string{kvbc::keyTypes::reconfiguration_install_key}, ts, false);
  LOG_INFO(getLogger(), "InstallCommand command block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::KeyExchangeCommand& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& rres) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, sequence_number, std::string{kvbc::keyTypes::reconfiguration_key_exchange}, ts, false);
  LOG_INFO(getLogger(), "KeyExchangeCommand command block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::AddRemoveCommand& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, sequence_number, std::string{kvbc::keyTypes::reconfiguration_add_remove}, ts, false);
  LOG_INFO(getLogger(), "AddRemoveCommand command block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::AddRemoveWithWedgeCommand& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  ver_updates.addUpdate(std::string{kvbc::keyTypes::reconfiguration_add_remove, 0x1},
                        std::string(serialized_command.begin(), serialized_command.end()));
  auto epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key}, concordUtils::toBigEndianStringBuffer(epoch));

  // Inject an update for state transferred replicas
  std::map<uint64_t, std::string> token;
  for (const auto& t : command.token) token.insert(t);
  auto execute_key_prefix =
      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                  static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_EXECUTE_COMMAND)};
  for (uint64_t i = 0; i < ReplicaConfig::instance().numReplicas + ReplicaConfig::instance().numRoReplicas; i++) {
    concord::messages::ClientsAddRemoveExecuteCommand cmd;
    cmd.config_descriptor = command.config_descriptor;
    if (token.find(i) == token.end()) continue;
    cmd.token = token[i];
    cmd.restart = command.restart;
    std::vector<uint8_t> serialized_cmd_data;
    concord::messages::serialize(serialized_cmd_data, cmd);
    // CRE will get this command and execute it
    ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key} + execute_key_prefix + std::to_string(i),
                          concordUtils::toBigEndianStringBuffer(epoch));
    ver_updates.addUpdate(execute_key_prefix + std::to_string(i),
                          std::string(serialized_cmd_data.begin(), serialized_cmd_data.end()));
  }
  auto blockId = persistReconfigurationBlock(ver_updates, sequence_number, ts, true);
  // update reserved pages for RO replica
  auto epochNum = bftEngine::EpochManager::instance().getSelfEpochNumber();
  auto wedgePoint = (sequence_number + 2 * checkpointWindowSize);
  wedgePoint = wedgePoint - (wedgePoint % checkpointWindowSize);
  concord::messages::ReconfigurationRequest rreqWithoutSignature;
  rreqWithoutSignature.command = command;
  bftEngine::ReconfigurationCmd::instance().saveReconfigurationCmdToResPages(
      rreqWithoutSignature,
      std::string{kvbc::keyTypes::reconfiguration_add_remove, 0x1},
      blockId,
      wedgePoint,
      epochNum);

  LOG_INFO(getLogger(), "AddRemove configuration command block is " << blockId);

  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::RestartCommand& command,
                                    uint64_t bft_seq_num,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, bft_seq_num, std::string{kvbc::keyTypes::reconfiguration_restart_key}, ts, true);
  LOG_INFO(getLogger(), "RestartCommand block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::AddRemoveStatus& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& response) {
  auto res = ro_storage_.getLatest(concord::kvbc::categorization::kConcordReconfigurationCategoryId,
                                   std::string{kvbc::keyTypes::reconfiguration_add_remove});
  if (res.has_value()) {
    auto strval = std::visit([](auto&& arg) { return arg.data; }, *res);
    concord::messages::AddRemoveCommand cmd;
    std::vector<uint8_t> bytesval(strval.begin(), strval.end());
    concord::messages::deserialize(bytesval, cmd);
    concord::messages::AddRemoveStatusResponse addRemoveResponse;
    addRemoveResponse.reconfiguration = cmd.reconfiguration;
    LOG_INFO(getLogger(), "AddRemoveCommand response: " << addRemoveResponse.reconfiguration);
    response.response = std::move(addRemoveResponse);
  } else {
    concord::messages::ReconfigurationErrorMsg error_msg;
    error_msg.error_msg = "key_not_found";
    response.response = std::move(error_msg);
    LOG_INFO(getLogger(), "AddRemoveCommand key not found");
    return false;
  }
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::AddRemoveWithWedgeStatus& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& response) {
  auto res = ro_storage_.getLatest(concord::kvbc::categorization::kConcordReconfigurationCategoryId,
                                   std::string{kvbc::keyTypes::reconfiguration_add_remove, 0x1});
  if (res.has_value()) {
    auto strval = std::visit([](auto&& arg) { return arg.data; }, *res);
    concord::messages::AddRemoveWithWedgeCommand cmd;
    std::vector<uint8_t> bytesval(strval.begin(), strval.end());
    concord::messages::deserialize(bytesval, cmd);
    concord::messages::AddRemoveWithWedgeStatusResponse addRemoveResponse;
    if (std::holds_alternative<concord::messages::AddRemoveWithWedgeStatusResponse>(response.response)) {
      addRemoveResponse = std::get<concord::messages::AddRemoveWithWedgeStatusResponse>(response.response);
    }
    addRemoveResponse.config_descriptor = cmd.config_descriptor;
    addRemoveResponse.restart_flag = cmd.restart;
    addRemoveResponse.bft_flag = cmd.bft_support;
    LOG_INFO(getLogger(), "AddRemoveWithWedgeCommand response: " << addRemoveResponse.config_descriptor);
    response.response = std::move(addRemoveResponse);
  } else {
    concord::messages::ReconfigurationErrorMsg error_msg;
    error_msg.error_msg = "key_not_found";
    response.response = std::move(error_msg);
    LOG_INFO(getLogger(), "AddRemoveWithWedgeCommand key not found");
    return false;
  }
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::PruneRequest& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command, sequence_number, std::string{kvbc::keyTypes::reconfiguration_pruning_key, 0x1}, ts, false);
  LOG_INFO(getLogger(), "PruneRequest configuration command block is " << blockId);
  return true;
}

// This is one way to trigger the compaction outside of a snapshot if the system is idle
// An optimization could be to release the snapshot in the PruneRequest handler
// and call compaction immediately after.
bool ReconfigurationHandler::handle(const concord::messages::PruneCompactRequest& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(serialized_command,
                                             sequence_number,
                                             std::string{kvbc::keyTypes::reconfiguration_prune_compact_key, 0x1},
                                             ts,
                                             false);
  LOG_INFO(getLogger(), "PruneCompactRequest configuration command block is " << blockId);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::ClientKeyExchangeCommand& command,
                                    uint64_t sequence_number,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& response) {
  std::vector<uint32_t> target_clients;
  for (auto& cid : command.target_clients) {
    target_clients.push_back(cid);
  }
  if (target_clients.empty()) {
    LOG_INFO(getLogger(), "exchange client keys for all clients");
    // We don't want to assume anything about the CRE client id. Hence, we write the update to all clients.
    // However, only the CRE client will be able to execute the requests.
    for (const auto& cg : ReplicaConfig::instance().clientGroups) {
      for (auto cid : cg.second) {
        target_clients.push_back(cid);
      }
    }
  }
  std::ostringstream oss;
  std::copy(target_clients.begin(), target_clients.end(), std::ostream_iterator<std::uint64_t>(oss, " "));
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto key_prefix = std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                                static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_KEY_EXCHANGE_COMMAND)};
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  concord::messages::ClientKeyExchangeCommandResponse ckecr;
  auto epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  for (auto clientid : target_clients) {
    ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key} + key_prefix + std::to_string(clientid),
                          concordUtils::toBigEndianStringBuffer(epoch));
    ver_updates.addUpdate(key_prefix + std::to_string(clientid),
                          std::string(serialized_command.begin(), serialized_command.end()));
  }
  ckecr.block_id = persistReconfigurationBlock(ver_updates, sequence_number, ts, false);
  LOG_INFO(getLogger(), "target clients: [" << oss.str() << "] block: " << ckecr.block_id);
  response.response = ckecr;
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::ClientsAddRemoveCommand& command,
                                    uint64_t sequence_number,
                                    uint32_t sender_id,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& response) {
  std::vector<uint32_t> target_clients;
  // ClientsAddRemoveCommand has optional list of <clientId, token>, we write update config descriptor and
  // and token Id relevant to the client id
  std::map<uint64_t, std::string> token;
  for (const auto& t : command.token) token.insert(t);

  for (const auto& cg : ReplicaConfig::instance().clientGroups) {
    for (auto cid : cg.second) {
      target_clients.push_back(cid);
    }
  }
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto key_prefix = std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                                static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_COMMAND)};
  auto execute_key_prefix =
      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                  static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_SCALING_EXECUTE_COMMAND)};
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  ver_updates.addUpdate(std::move(key_prefix), std::string(serialized_command.begin(), serialized_command.end()));
  auto epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  for (auto clientid : target_clients) {
    concord::messages::ClientsAddRemoveExecuteCommand cmd;
    cmd.config_descriptor = command.config_descriptor;
    if (token.find(clientid) != token.end()) cmd.token = token[clientid];
    cmd.restart = command.restart;
    std::vector<uint8_t> serialized_cmd_data;
    concord::messages::serialize(serialized_cmd_data, cmd);
    // CRE will get this command and execute it
    ver_updates.addUpdate(execute_key_prefix + std::to_string(clientid),
                          std::string(serialized_cmd_data.begin(), serialized_cmd_data.end()));
    ver_updates.addUpdate(
        std::string{keyTypes::reconfiguration_epoch_key} + execute_key_prefix + std::to_string(clientid),
        concordUtils::toBigEndianStringBuffer(epoch));
  }
  ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key}, concordUtils::toBigEndianStringBuffer(epoch));
  auto block_id = persistReconfigurationBlock(ver_updates, sequence_number, ts, false);
  LOG_INFO(getLogger(), "ClientsAddRemoveCommand block_id is: " << block_id);
  return true;
}
bool ReconfigurationHandler::handle(const concord::messages::ClientsRestartCommand& command,
                                    uint64_t bft_seq_num,
                                    uint32_t sender_id,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  std::vector<uint32_t> target_clients;
  for (const auto& cg : ReplicaConfig::instance().clientGroups) {
    for (auto cid : cg.second) {
      target_clients.push_back(cid);
    }
  }
  auto key_prefix = std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                                static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_RESTART_COMMAND)};
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  ver_updates.addUpdate(std::string(key_prefix), std::string(serialized_command.begin(), serialized_command.end()));
  auto epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  for (auto clientid : target_clients) {
    ver_updates.addUpdate(key_prefix + std::to_string(clientid),
                          std::string(serialized_command.begin(), serialized_command.end()));
    ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key} + key_prefix + std::to_string(clientid),
                          concordUtils::toBigEndianStringBuffer(epoch));
  }
  auto block_id = persistReconfigurationBlock(ver_updates, bft_seq_num, ts, false);

  LOG_INFO(getLogger(), "Client RestartCommand block is " << block_id);
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::ClientsRestartStatus& command,
                                    uint64_t bft_seq_num,
                                    uint32_t sender_id,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& rres) {
  concord::messages::ClientsRestartStatusResponse stats;
  for (const auto& gr : ReplicaConfig::instance().clientGroups) {
    for (auto cid : gr.second) {
      std::string key = std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                                    static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_RESTART_STATUS)} +
                        std::to_string(cid);
      auto bid = ro_storage_.getLatestVersion(concord::kvbc::categorization::kConcordReconfigurationCategoryId, key);
      if (bid.has_value()) {
        auto saved_ts = get(std::string{kvbc::keyTypes::reconfiguration_ts_key}, bid.value().version, ro_storage_);
        uint64_t numeric_ts{0};
        if (saved_ts.has_value()) {
          auto strval = std::visit([](auto&& arg) { return arg.data; }, *saved_ts);
          numeric_ts = concordUtils::fromBigEndianBuffer<uint64_t>(strval.data());
          stats.timestamps.push_back(std::make_pair(cid, numeric_ts));
        }
      }
    }
  }
  rres.response = stats;
  return true;
}
bool KvbcClientReconfigurationHandler::handle(const concord::messages::ClientsRestartUpdate& command,
                                              uint64_t bft_seq_num,
                                              uint32_t sender_id,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command,
      bft_seq_num,
      std::string{kvbc::keyTypes::reconfiguration_client_data_prefix,
                  static_cast<char>(kvbc::keyTypes::CLIENT_COMMAND_TYPES::CLIENT_RESTART_STATUS)} +
          std::to_string(sender_id),
      ts,
      false);
  LOG_INFO(getLogger(), "block id: " << KVLOG(blockId, sender_id));
  return true;
}

bool ReconfigurationHandler::handle(const messages::UnwedgeCommand& cmd,
                                    uint64_t bft_seq_num,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse&) {
  if (!bftEngine::ControlStateManager::instance().isWedged()) {
    LOG_INFO(getLogger(), "replica is already unwedge");
    return true;
  }
  LOG_INFO(getLogger(), "Unwedge command started " << KVLOG(cmd.bft_support));
  auto curr_epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  auto quorum_size = ReplicaConfig::instance().numReplicas;
  if (cmd.bft_support) quorum_size = 2 * ReplicaConfig::instance().fVal + ReplicaConfig::instance().cVal + 1;
  uint32_t valid_sigs{0};
  for (auto const& [id, unwedge_stat] : cmd.unwedges) {
    if (unwedge_stat.curr_epoch < curr_epoch) continue;
    std::string sig_data = std::to_string(id) + std::to_string(unwedge_stat.curr_epoch);
    auto& sig = unwedge_stat.signature;
    std::string signature(sig.begin(), sig.end());
    bool valid = bftEngine::impl::SigManager::instance()->verifySig(id, sig_data, signature);
    if (valid) valid_sigs++;
  }
  LOG_INFO(getLogger(), "verified " << valid_sigs << " unwedge signatures, required quorum is " << quorum_size);
  bool can_unwedge = (valid_sigs >= quorum_size);
  if (can_unwedge) {
    if (!cmd.restart) {
      auto bid = persistNewEpochBlock(bft_seq_num);
      persistLastBlockIdInMetadata<false>(bid, persistent_storage_);
      bftEngine::ControlStateManager::instance().setStopAtNextCheckpoint(0);
      bftEngine::ControlStateManager::instance().unwedge();
      bftEngine::IControlHandler::instance()->resetState();
      LOG_INFO(getLogger(), "Unwedge command completed successfully");
    } else {
      bftEngine::EpochManager::instance().setNewEpochFlag(true);
      bftEngine::ControlStateManager::instance().restart();
    }
  }
  return can_unwedge;
}

bool ReconfigurationHandler::handle(const messages::UnwedgeStatusRequest& req,
                                    uint64_t,
                                    uint32_t,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& rres) {
  concord::messages::UnwedgeStatusResponse response;
  response.replica_id = ReplicaConfig::instance().replicaId;
  if (bftEngine::ControlStateManager::instance().getCheckpointToStopAt().has_value()) {
    if ((!req.bft_support && !bftEngine::IControlHandler::instance()->isOnNOutOfNCheckpoint()) ||
        (req.bft_support && !bftEngine::IControlHandler::instance()->isOnStableCheckpoint())) {
      response.can_unwedge = false;
      response.reason = "replica is not at wedge point";
      rres.response = response;
      return true;
    }
  }
  auto curr_epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  std::string sig_data = std::to_string(ReplicaConfig::instance().getreplicaId()) + std::to_string(curr_epoch);
  auto sig_manager = bftEngine::impl::SigManager::instance();
  std::string sig(sig_manager->getMySigLength(), '\0');
  sig_manager->sign(sig_data.c_str(), sig_data.size(), sig.data());
  response.can_unwedge = true;
  response.curr_epoch = curr_epoch;
  response.signature = std::vector<uint8_t>(sig.begin(), sig.end());
  LOG_INFO(getLogger(), "Replica is ready to unwedge " << KVLOG(curr_epoch));
  rres.response = response;
  return true;
}

bool ReconfigurationHandler::handle(const concord::messages::PruneStatusRequest& command,
                                    uint64_t bft_seq_num,
                                    uint32_t sender_id,
                                    const std::optional<bftEngine::Timestamp>& ts,
                                    concord::messages::ReconfigurationResponse& rres) {
  if (std::holds_alternative<concord::messages::ReconfigurationErrorMsg>(rres.response)) return rres.success;
  if (!std::holds_alternative<concord::messages::PruneStatus>(rres.response)) {
    rres.response = concord::messages::PruneStatus{};
  }
  return true;
}

bool InternalKvReconfigurationHandler::verifySignature(uint32_t sender_id,
                                                       const std::string& data,
                                                       const std::string& signature) const {
  if (sender_id >= ReplicaConfig::instance().numReplicas) return false;
  return bftEngine::impl::SigManager::instance()->verifySig(sender_id, data, signature);
}

bool InternalKvReconfigurationHandler::handle(const concord::messages::ReplicaMainKeyUpdate& command,
                                              uint64_t bft_seq_num,
                                              uint32_t sender_id,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);

  auto blockId =
      persistReconfigurationBlock(serialized_command,
                                  bft_seq_num,
                                  std::string{kvbc::keyTypes::reconfiguration_rep_main_key} + std::to_string(sender_id),
                                  ts,
                                  false);
  auto signatureAlgorithmId = static_cast<uint32_t>(command.algorithm);
  LOG_INFO(getLogger(),
           "Persisted ReplicaMainKeyUpdate on chain" << KVLOG(sender_id, bft_seq_num, blockId, signatureAlgorithmId));
  return true;
}
bool InternalKvReconfigurationHandler::handle(const concord::messages::WedgeCommand& command,
                                              uint64_t bft_seq_num,
                                              uint32_t,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse&) {
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  if (command.noop) {
    auto seq_num_to_stop_at = bftEngine::ControlStateManager::instance().getCheckpointToStopAt();
    if (!seq_num_to_stop_at.has_value() || bft_seq_num > seq_num_to_stop_at) {
      LOG_ERROR(getLogger(), "Invalid noop wedge command, it won't be writen to the blockchain");
      return false;
    }
    auto blockId = persistReconfigurationBlock(
        serialized_command, bft_seq_num, std::string{kvbc::keyTypes::reconfiguration_wedge_key, 0x1}, ts, false);
    LOG_INFO(getLogger(), "received noop command, a new block will be written" << KVLOG(bft_seq_num, blockId));
    return true;
  }
  return false;
}

bool InternalKvReconfigurationHandler::handle(const concord::messages::ReplicaTlsExchangeKey& command,
                                              uint64_t sequence_number,
                                              uint32_t sender_id,
                                              const std::optional<bftEngine::Timestamp>& ts,
                                              concord::messages::ReconfigurationResponse& response) {
  if (command.sender_id != sender_id) {
    concord::messages::ReconfigurationErrorMsg error_msg;
    error_msg.error_msg = "sender_id of the message does not match the real sender id";
    response.response = error_msg;
    return false;
  }
  std::vector<uint8_t> serialized_command;
  concord::messages::serialize(serialized_command, command);
  auto blockId = persistReconfigurationBlock(
      serialized_command,
      sequence_number,
      std::string{kvbc::keyTypes::reconfiguration_tls_exchange_key} + std::to_string(sender_id),
      ts,
      false);
  LOG_INFO(getLogger(), "ReplicaTlsExchangeKey block id: " << blockId << " for replica " << sender_id);
  return true;
}

bool InternalPostKvReconfigurationHandler::handle(const concord::messages::ClientExchangePublicKey& command,
                                                  uint64_t sequence_number,
                                                  uint32_t sender_id,
                                                  const std::optional<bftEngine::Timestamp>& ts,
                                                  concord::messages::ReconfigurationResponse& response) {
  concord::kvbc::categorization::VersionedUpdates ver_updates;
  auto updated_client_keys = SigManager::instance()->getClientsPublicKeys();
  auto epoch = bftEngine::EpochManager::instance().getSelfEpochNumber();
  std::string command_key = std::string(1, concord::kvbc::kClientsPublicKeys);
  ver_updates.addUpdate(std::string(1, concord::kvbc::kClientsPublicKeys),
                        concordUtils::toBigEndianStringBuffer(epoch));
  ver_updates.addUpdate(std::string{keyTypes::reconfiguration_epoch_key}, concordUtils::toBigEndianStringBuffer(epoch));
  ver_updates.addUpdate(std::string(1, concord::kvbc::kClientsPublicKeys), std::string(updated_client_keys));
  ver_updates.addUpdate(
      std::string{keyTypes::reconfiguration_epoch_key} + std::string(1, concord::kvbc::kClientsPublicKeys),
      concordUtils::toBigEndianStringBuffer(epoch));
  auto id = persistReconfigurationBlock(ver_updates, sequence_number, ts, false);
  LOG_INFO(getLogger(),
           "Writing client keys to block [" << id << "] after key exchange, keys "
                                            << std::hash<std::string>{}(updated_client_keys));
  if (!ReplicaConfig::instance().saveClinetKeyFile) return true;
  // Now that keys have exchanged, lets persist the new key in the file system
  uint32_t group_id = 0;
  for (const auto& [gid, cgr] : ReplicaConfig::instance().clientGroups) {
    if (std::find(cgr.begin(), cgr.end(), sender_id) != cgr.end()) {
      group_id = gid;
      break;
    }
  }
  std::string path =
      ReplicaConfig::instance().clientsKeysPrefix + "/" + std::to_string(group_id) + "/transaction_signing_pub.pem";
  std::pair<std::string, std::string> pem_key;
  if (ReplicaConfig::instance().replicaMsgSigningAlgo == SignatureAlgorithm::EdDSA) {
    pem_key = EdDSAHexToPem(std::make_pair("", command.pub_key));
  }

  concord::secretsmanager::SecretsManagerPlain sm;
  LOG_INFO(getLogger(), KVLOG(path, pem_key.second, sender_id));
  return sm.encryptFile(path, pem_key.second);
}

}  // namespace concord::kvbc::reconfiguration
