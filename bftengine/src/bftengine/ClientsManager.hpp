// Concord
//
// Copyright (c) 2018-2021 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").  You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#pragma once

#include "PrimitiveTypes.hpp"
#include "TimeUtils.hpp"
#include "bftengine/ReservedPagesClient.hpp"
#include "util/Metrics.hpp"
#include "IPendingRequest.hpp"
#include "bftengine/IKeyExchanger.hpp"
#include "PersistentStorage.hpp"
#include "ReplicaSpecificInfoManager.hpp"
#include <mutex>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <queue>
#include <boost/bimap.hpp>

namespace bftEngine {
class IStateTransfer;

namespace impl {
class ClientReplyMsg;
class ClientRequestMsg;

// Keeps track of Client IDs, public keys, and pending requests and replies. Supports saving and loading client public
// keys and pending reply messages to the reserved pages mechanism.
//
// Not thread-safe.
class ClientsManager : public ResPagesClient<ClientsManager>, public IPendingRequest, public IClientPublicKeyStore {
 public:
  // As preconditions to this constructor:
  //   - The ReplicaConfig singleton (i.e. ReplicaConfig::instance()) must be initialized with the relevant
  //     configuration.
  //   - The reserved pages mechanism must be initialized and usable.
  //   - The global logger CL_MNGR must be initialized.
  // Behavior is undefined if any of these preconditions are not met. Behavior is also undefined if proxyClients,
  // externalClients, and internalClients are all empty.
  // Additionally, all current and future behavior of the constructed ClientsManager object becomes undefined if any of
  // the following conditions occur:
  //   - The reserved pages mechanism stops being usable.
  //   - The concordMetrics::Component object referenced by metrics is destroyed.
  //   - The global logger CL_MNGR is destroyed.
  ClientsManager(const std::set<NodeIdType>& proxyClients,
                 const std::set<NodeIdType>& externalClients,
                 const std::set<NodeIdType>& clientServices,
                 const std::set<NodeIdType>& internalClients,
                 concordMetrics::Component& metrics);
  ClientsManager(std::shared_ptr<PersistentStorage> ps,
                 const std::set<NodeIdType>& proxyClients,
                 const std::set<NodeIdType>& externalClients,
                 const std::set<NodeIdType>& clientServices,
                 const std::set<NodeIdType>& internalClients,
                 concordMetrics::Component& metrics);

  uint32_t numberOfRequiredReservedPages() const { return clientIds_.size() * reservedPagesPerClient_; }

  // Loads any available client public keys and client reply records from the reserved pages. Saves any client public
  // keys loaded from the reserved pages to the KeyExchangeManager singleton. Automatically deletes the oldest reply
  // record for a client if a reply message is found in the reserved pages for that client but the ClientsManager
  // already has a number of reply records for that client equalling or exceeding the maximum client batch size that
  // was configured at the time of this ClientManager's construction (or 1 if client batching was disabled). If the
  // ClientsManager already has existing reply records matching the client ID and sequence number of a reply found in
  // the reserved pages, the existing record will be overwritten. Automatically deletes any request records for a given
  // client with sequence numbers less than or equal to the sequence number of a reply to that client found in the
  // reserved pages. As a precondition to this function, the KeyExchangeManager singleton
  // (KeyExchangeManager::instance()) must be fully initialized and usable. Behavior is undefined if it is not, and is
  // also undefined if the applicable reserved pages contain malformed data.
  void loadInfoFromReservedPages();

  // Replies

  // Returns true if clientId belongs to a valid client and this ClientsManager currently has a record for a reply to
  // that client with ID reqSeqNum. Returns false otherwise.
  // TODO(GG): make sure that ReqId is based on time (and ignore requests with time that does
  // not make sense (too high) - this will prevent some potential attacks)
  bool hasReply(NodeIdType clientId, ReqId reqSeqNum);

  bool isValidClient(NodeIdType clientId) const { return clientIds_.find(clientId) != clientIds_.end(); }

  // First, if this ClientsManager has a number of reply records for the given clientId equalling or exceeding the
  // maximum client batch size configured at the time of this ClientManager's construction (or 1 if client batching was
  // not enabled), deletes the oldest such record. Then, a ClientReplyMsg is allocated with the given sequence number
  // and payload, and a copy of the message is saved to the reserved pages (overwriting any existing reply for clientId
  // in the reserved pages), and this ClientManager adds a record for this reply (potentially replacing any existing
  // record for the given sequence number). Returns the allocated ClientReplyMsg. Behavior is undefined for all of the
  // following cases:
  // - clientId does not belong to a valid client.
  // - The number of reply records this ClientsManager has for the given client is above the maximum even after the
  //   oldest one is deleted.
  // - The size of the allocated reply message exceeds the maximum reply size that was configured at the time of this
  //   ClientsManager's construction.
  std::unique_ptr<ClientReplyMsg> allocateNewReplyMsgAndWriteToStorage(NodeIdType clientId,
                                                                       ReqId requestSeqNum,
                                                                       uint16_t currentPrimaryId,
                                                                       char* reply,
                                                                       uint32_t replyLength,
                                                                       uint16_t reqIndexInBatch,
                                                                       uint32_t rsiLength,
                                                                       uint32_t executionResult = 0);

  // Loads a client reply message from the reserved pages, and allocates and returns a ClientReplyMsg containing the
  // loaded message. Returns a null pointer if the configuration recorded at the time of this ClientManager's
  // construction enabled client batching with a maximum batch size greater than 1 and the message loaded from the
  // reserved pages has a sequence number not matching requestSeqNum. Behavior is undefined for all of the following
  // cases:
  // - clientId does not belong to a valid client.
  // - The reserved pages do not contain client reply message data of the expected format for clientId.
  // - The configuration recorded at the time of this ClientsManager's construction did not enable client batching or
  //   enabled it with a maximum batch size of 1, but the sequence number of the reply loaded from the reserved pages
  //   does not match requestSeqNum.
  std::unique_ptr<ClientReplyMsg> allocateReplyFromSavedOne(NodeIdType clientId,
                                                            ReqId requestSeqNum,
                                                            uint16_t currentPrimaryId);

  // Requests

  // Returns true if there is a valid client with ID clientId and this ClientsManager currently has a recorded request
  // with ID reqSeqNum from that client; otherwise returns false.
  bool isClientRequestInProcess(NodeIdType clientId, ReqId reqSeqNum);

  // Returns true IFF there is no pending requests for clientId, and reqSeqNum can become the new pending request, that
  // is, if all of the following are true:
  // - clientId belongs to a valid client.
  // - The number of requests this ClientsManager currently has recorded for that client is not exactly equal to the
  //   maximum client batch size configured at the time of this ClientsManager's construction (or 1 if client batching
  //   was not enabled).
  // - This ClientsManager does not already have any request or reply associated with that client recorded with ID
  //   matching reqSeqNum.
  // otherwise returns false.
  bool canBecomePending(NodeIdType clientId, ReqId reqSeqNum) const;

  // Returns true if there is a valid client with ID clientId, this ClientsManager currently has a recorded request with
  // ID reqSeqNum from that client, and that request has not been marked as committed; otherwise returns false.
  bool isPending(NodeIdType clientId, ReqId reqSeqNum) const override;

  // Adds a record for the request with reqSeqNum from the client with the given clientId (if a record for that request
  // does not already exist). Behavior is undefined if clientId does not belong to a valid client.
  void addPendingRequest(NodeIdType clientId, ReqId reqSeqNum, const std::string& cid);

  // Mark a request with ID reqSequenceNum that this ClientsManager currently has recorded as committed (does nothing if
  // there is no existing record for reqSequenceNum). Behavior is undefined if clientId does not belong to a valid
  // client.
  void markRequestAsCommitted(NodeIdType clientId, ReqId reqSequenceNum);

  // Removes the current request record from the given client with the greatest sequence number if both of the following
  // are true:
  // - That greatest sequence number is greater than reqSequenceNum.
  // - There is no current request record for the client with ID clientId and sequence number reqSequenceNum.
  // - The number of requests this ClientsManager currently has recorded for the given client is exactly equal to the
  //   global system constant maxNumOfRequestsInBatch (note this is not the same quantity as the maximum configured
  //   client batch size).
  // Does nothing otherwise. Behavior is undefined if clientId does not belong to a valid client.
  void removeRequestsOutOfBatchBounds(NodeIdType clientId, ReqId reqSequenceNum);

  // If clientId belongs to a valid client and this ClientsManager currently has a request recorded with reqSeqNum,
  // removes the record for that request. Does nothing otherwise.
  void removePendingForExecutionRequest(NodeIdType clientId, ReqId reqSeqNum);

  // Removes all request records this ClientsManager currently has.
  void clearAllPendingRequests();

  // Finds the request recorded by this ClientsManager at the earliest time (ignoring requests marked as committed),
  // writes its CID to the reference cid, and returns what that earliest time was. Writes an empty string to cid and
  // returns bftEngine::impl::MaxTime if this ClientsManager does not currently have records for any non-committed
  // requests.
  Time infoOfEarliestPendingRequest(std::string& cid) const;

  // Log a message for each request not marked as committed that this ClientsManager currently has a record for created
  // at a time more than threshold milliseconds before currTime. As a precondition to this function, the global logger
  // VC_LOG must be initialized. Behavior is undefined if it is not.
  void logAllPendingRequestsExceedingThreshold(const int64_t threshold, const Time& currTime) const;

  // Deletes the reply to clientId this ClientsManager currently has a record for at same index in batch. There should
  // be at most 1 reply at the index saved for each client. Does nothing if this ClientsManager has no reply records to
  // the given clientId and index. Behavior is undefined if clientId does not belong to a valid client.
  void deleteReplyIfNeeded(NodeIdType clientId, uint16_t indexInBatch, ReqId newReqSeqNum = 0);

  // Sets/updates a client public key and persist it to the reserved pages. Behavior is undefined in the following
  // cases:
  //   - The given NodeIdType parameter is not the ID of a valid client.
  //   - The given public key does not fit in a single reserved page under ClientsManager's implementation.
  void setClientPublicKey(NodeIdType, const std::string& key, concord::crypto::KeyFormat) override;

  // General
  static uint32_t reservedPagesPerRequest(uint32_t sizeOfReservedPage, uint32_t maxReplySize);
  static uint32_t reservedPagesPerClient(uint32_t sizeOfReservedPage,
                                         uint32_t maxReplySize,
                                         uint16_t maxNumReqPerClient);

  bool isInternal(NodeIdType clientId) const;

 protected:
  uint32_t getReplyFirstPageId(NodeIdType clientId) const { return getKeyPageId(clientId) + 1; }

  uint32_t getKeyPageId(NodeIdType clientId) const {
    return clientIdsToReservedPages_.at(clientId) * reservedPagesPerClient_;
  }

  const ReplicaId myId_;

  std::string scratchPage_;

  uint32_t reservedPagesPerRequest_;
  uint32_t reservedPagesPerClient_;

  struct RequestInfo {
    RequestInfo() : time(MinTime) {}
    RequestInfo(Time t, const std::string& c) : time(t), cid(c) {}

    Time time;
    std::string cid;
    bool committed = false;
  };

  class RequestsInfo {
   public:
    void emplaceSafe(NodeIdType clientId, ReqId reqSeqNum, const std::string& cid);
    bool removeRequestsOutOfBatchBoundsSafe(NodeIdType clientId, ReqId reqSequenceNum);
    bool findSafe(ReqId reqSeqNum);
    void clearSafe();
    void removeOldPendingReqsSafe(NodeIdType clientId, ReqId reqSeqNum);
    void removePendingForExecutionRequestSafe(NodeIdType clientId, ReqId reqSeqNum);

    size_t size() const { return requestsMap_.size(); }
    bool find(ReqId reqSeqNum) const;
    bool isPending(ReqId reqSeqNum) const;
    void markRequestAsCommitted(NodeIdType clientId, ReqId reqSeqNum);
    void infoOfEarliestPendingRequest(Time& earliestTime, RequestInfo& earliestPendingReqInfo) const;
    void logAllPendingRequestsExceedingThreshold(const int64_t threshold,
                                                 const Time& currTime,
                                                 int& numExceeding) const;

   public:
    std::mutex requestsMapMutex_;
    std::map<ReqId, RequestInfo> requestsMap_;
  };

  class RepliesInfo {
   public:
    // The thread safety here is between the PreProcessor thread, which is read-only, and the ReplicaImp main thread,
    // which performs either read or write at a time. So, all write operations should be guarded as well as PrePropessor
    // calls hasReply and isClientRequestInProcess. All other operations are safe.
    void deleteReplyIfNeededSafe(NodeIdType clientId,
                                 ReqId reqSeqNum,
                                 uint16_t maxNumOfReqsPerClient,
                                 uint16_t reqIndex);
    void insertOrAssignSafe(ReqId reqSeqNum, uint16_t reqIndexInBatch);
    bool findSafe(ReqId reqSeqNum);

    bool find(ReqId reqSeqNum) const;
    uint16_t getIndex(ReqId reqSeqNum) const;

   public:
    std::mutex repliesMapMutex_;
    // repliesBiMap_ maps request sequence number to request index in client batch and vice versa.
    // We use the seqNum to index map to calculate the page of a saved reply in the reserved pages,
    // and we use the index as a key when we delete a reply record that is about to be overridden.
    boost::bimaps::bimap<ReqId, uint16_t> repliesBiMap_;
  };

  struct ClientInfo {
    std::shared_ptr<RequestsInfo> requestsInfo;
    std::shared_ptr<RepliesInfo> repliesInfo;
    std::pair<std::string, concord::crypto::KeyFormat> pubKey;
  };

  std::set<NodeIdType> proxyClients_;
  std::set<NodeIdType> externalClients_;
  std::set<NodeIdType> clientServices_;
  std::set<NodeIdType> internalClients_;
  std::set<NodeIdType> clientIds_;
  std::map<NodeIdType, uint32_t> clientIdsToReservedPages_;
  std::unordered_map<NodeIdType, ClientInfo> clientsInfo_;
  const uint32_t maxReplySize_;
  const uint16_t maxNumOfReqsPerClient_;
  concordMetrics::Component& metrics_;
  concordMetrics::CounterHandle metric_reply_inconsistency_detected_;
  concordMetrics::CounterHandle metric_removed_due_to_out_of_boundaries_;
  std::unique_ptr<RsiDataManager> rsiManager_;
};  // namespace impl

}  // namespace impl
}  // namespace bftEngine
