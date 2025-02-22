// UTT Client API
//
// Copyright (c) 2020-2022 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").
// You may not use this product except in compliance with the Apache 2.0
// License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the sub-component's license, as noted in the LICENSE
// file.

#pragma once
#include <vector>
#include "coin.hpp"

namespace utt::client {
class IStorage {
 public:
  class tx_guard {
   public:
    tx_guard(IStorage& storage) : storage_{storage} { storage_.startTransaction(); }
    ~tx_guard() { storage_.commit(); }

   private:
    IStorage& storage_;
  };

  virtual ~IStorage() = default;

  /**
   * @brief Indicates if the storage is initialized or not
   *
   * @return true if not initialized
   * @return false if initialized
   */
  virtual bool isNewStorage() = 0;

  /**
   * @brief Set the the user's private and public keys to the storage
   *
   * @param key_pair a pair of serialized <private key, public key>
   */
  virtual void setKeyPair(const std::pair<std::string, std::string>& key_pair) = 0;

  /**
   * @brief Set the Client Side Secret (s1) generated by the utt library
   *
   * @param s1 a curve point (vector<uint64_t>) representing the client side secret (s1)
   */
  virtual void setClientSideSecret(const libutt::api::types::CurvePoint& s1) = 0;

  /**
   * @brief Set the System Side Secret (s2) generated by the utt replicas
   *
   * @param s2 a curve point (vector<uint64_t>) representing the replicas side secret (s2)
   */
  virtual void setSystemSideSecret(const libutt::api::types::CurvePoint& s2) = 0;

  /**
   * @brief Set the Rcm Signature, this signature is generated by the utt replicas (and collected by the user)
   *
   * @param sig the serialized rcm signature (vector<uint8_t>)
   */
  virtual void setRcmSignature(const libutt::api::types::Signature& sig) = 0;

  /**
   * @brief Set a new utt coin.
   *
   * @param coin a utt coin object
   */
  virtual void setCoin(const libutt::api::Coin& coin) = 0;

  /**
   * @brief Remove a utt coin from the storage
   *
   * @param coin a utt coin object
   */
  virtual void removeCoin(const libutt::api::Coin& coin) = 0;

  /**
   * @brief Get a curve point representing the client side secret (s1) as stored by setClientSideSecret
   *
   * @return libutt::api::types::CurvePoint (vector<uint64_t>)
   */
  virtual libutt::api::types::CurvePoint getClientSideSecret() = 0;

  /**
   * @brief Get a curve point representing the replicas side secret (s2) as stored by setSystemSideSecret
   *
   * @return libutt::api::types::CurvePoint (vector<uint64_t>)
   */
  virtual libutt::api::types::CurvePoint getSystemSideSecret() = 0;

  /**
   * @brief Get the serialized rcm signature as stored by setRcmSignature
   *
   * @return libutt::api::types::Signature (vector<uint8_t>)
   */
  virtual libutt::api::types::Signature getRcmSignature() = 0;

  /**
   * @brief Get all of the current stored utt coins
   *
   * @return std::vector<libutt::api::Coin>
   */
  virtual std::vector<libutt::api::Coin> getCoins() = 0;

  /**
   * @brief Get the a pair of the user's serialized key pair
   *
   * @return std::pair<std::string, std::string> where the first is the private key and the second is the public key
   */
  virtual std::pair<std::string, std::string> getKeyPair() = 0;

  /**
   * @brief Starts a new atomic transaction
   *
   */
  virtual void startTransaction() = 0;

  /**
   * @brief Atomically commits a transaction
   *
   */
  virtual void commit() = 0;
};
}  // namespace utt::client