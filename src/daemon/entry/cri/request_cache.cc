/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2017-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: tanyifeng
 * Create: 2017-11-22
 * Description: provide request cache function definition
 *********************************************************************************/
#include "request_cache.h"
#include <iostream>
#include <utility>
#include <chrono>
#include <thread>
#include <mutex>
#include <random>
#include <cmath>
#include <libwebsockets.h>
#include "isula_libutils/log.h"

std::atomic<RequestCache *> RequestCache::m_instance;
std::mutex RequestCache::m_mutex;
RequestCache *RequestCache::GetInstance() noexcept
{
    RequestCache *cache = m_instance.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (cache == nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        cache = m_instance.load(std::memory_order_relaxed);
        if (cache == nullptr) {
            cache = new RequestCache;
            std::atomic_thread_fence(std::memory_order_release);
            m_instance.store(cache, std::memory_order_relaxed);
        }
    }
    return cache;
}

std::string RequestCache::InsertExecRequest(const runtime::v1alpha2::ExecRequest &req)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Remove expired entries.
    GarbageCollection();
    // If the cache is full, reject the request.
    if (m_ll.size() == MaxInFlight) {
        ERROR("too many cache in flight!");
        return "";
    }
    auto token = UniqueToken();
    CacheEntry tmp;
    tmp.SetValue(token, &req, nullptr, std::chrono::system_clock::now() + std::chrono::minutes(1));
    m_ll.push_front(tmp);
    m_tokens.insert(std::make_pair(token, tmp));
    return token;
}

std::string RequestCache::InsertAttachRequest(const runtime::v1alpha2::AttachRequest &req)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Remove expired entries.
    GarbageCollection();
    // If the cache is full, reject the request.
    if (m_ll.size() == MaxInFlight) {
        ERROR("too many cache in flight!");
        return "";
    }
    auto token = UniqueToken();
    CacheEntry tmp;
    tmp.SetValue(token, nullptr, &req, std::chrono::system_clock::now() + std::chrono::minutes(1));
    m_ll.push_front(tmp);
    m_tokens.insert(std::make_pair(token, tmp));
    return token;
}

void RequestCache::GarbageCollection()
{
    auto now = std::chrono::system_clock::now();
    while (!m_ll.empty()) {
        CacheEntry oldest = m_ll.back();
        if (now < oldest.expireTime) {
            return;
        }
        m_ll.pop_back();
        m_tokens.erase(oldest.token);
    }
}

std::string RequestCache::UniqueToken()
{
    const int maxTries { 50 };
    std::random_device r;
    std::default_random_engine e1(r());
    std::uniform_int_distribution<int> uniform_dist(1, 254);
    // Number of bytes to be TokenLen when base64 encoded.
    const int tokenSize = ceil(static_cast<double>(TokenLen) * 6 / 8);
    char rawToken[tokenSize + 1];
    (void)memset(rawToken, 0, sizeof(rawToken));
    for (int i {}; i < maxTries; ++i) {
        char buf[TokenLen + 1];
        (void)memset(buf, 0, sizeof(buf));
        for (int j {}; j < tokenSize; ++j) {
            rawToken[j] = (char)uniform_dist(e1);
        }
        lws_b64_encode_string(rawToken, (int)strlen(rawToken), buf, (int)sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
        if (strlen(buf) < TokenLen) {
            continue;
        }
        std::string token(buf, buf + TokenLen);
        if (token.length() != TokenLen) {
            continue;
        }

        bool ok { true };
        std::string subDelims { R"(-._:~!$&'()*+,;/=%@)" };
        for (const auto &t : token) {
            if ((subDelims.find(t) != std::string::npos)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        auto it = m_tokens.find(token);
        if (it == m_tokens.end()) {
            return token;
        }
    }
    ERROR("create unique token failed!");
    return "";
}

bool RequestCache::IsValidToken(const std::string &token)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return static_cast<bool>(m_tokens.count(token));
}

// Consume the token (remove it from the cache) and return the cached request, if found.
runtime::v1alpha2::ExecRequest RequestCache::ConsumeExecRequest(const std::string &token)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_tokens.count(token) == 0 || m_tokens[token].execRequest.size() == 0) {
        ERROR("Invalid token");
        return runtime::v1alpha2::ExecRequest();
    }

    CacheEntry ele = m_tokens[token];
    for (auto it = m_ll.begin(); it != m_ll.end(); it++) {
        if (it->token == token) {
            m_ll.erase(it);
            break;
        }
    }
    m_tokens.erase(token);
    if (std::chrono::system_clock::now() > ele.expireTime) {
        return runtime::v1alpha2::ExecRequest();
    }

    return ele.execRequest.at(0);
}

runtime::v1alpha2::AttachRequest RequestCache::ConsumeAttachRequest(const std::string &token)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_tokens.count(token) == 0 || m_tokens[token].attachRequest.size() == 0) {
        ERROR("Invalid token");
        return runtime::v1alpha2::AttachRequest();
    }

    CacheEntry ele = m_tokens[token];
    for (auto it = m_ll.begin(); it != m_ll.end(); it++) {
        if (it->token == token) {
            m_ll.erase(it);
            break;
        }
    }
    m_tokens.erase(token);
    if (std::chrono::system_clock::now() > ele.expireTime) {
        return runtime::v1alpha2::AttachRequest();
    }

    return ele.attachRequest.at(0);
}