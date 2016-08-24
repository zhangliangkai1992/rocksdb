//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include "util/statistics.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include "rocksdb/statistics.h"
#include "port/likely.h"
#include <algorithm>
#include <cstdio>

namespace rocksdb {

std::shared_ptr<Statistics> CreateDBStatistics() {
  return std::make_shared<StatisticsImpl>(nullptr, false);
}

StatisticsImpl::StatisticsImpl(
    std::shared_ptr<Statistics> stats,
    bool enable_internal_stats)
  : stats_shared_(stats),
    stats_(stats.get()),
    enable_internal_stats_(enable_internal_stats) {
}

StatisticsImpl::~StatisticsImpl() {}

uint64_t StatisticsImpl::getTickerCount(uint32_t tickerType) const {
  MutexLock lock(&aggregate_lock_);
  assert(
    enable_internal_stats_ ?
      tickerType < INTERNAL_TICKER_ENUM_MAX :
      tickerType < TICKER_ENUM_MAX);
  uint64_t thread_local_sum = 0;
  tickers_[tickerType].thread_value->Fold(
      [](void* curr_ptr, void* res) {
        auto* sum_ptr = static_cast<uint64_t*>(res);
        *sum_ptr += static_cast<std::atomic_uint_fast64_t*>(curr_ptr)->load();
      },
      &thread_local_sum);
  return thread_local_sum + tickers_[tickerType].merged_sum.load();
}

void StatisticsImpl::histogramData(uint32_t histogramType,
                                   HistogramData* const data) const {
  assert(
    enable_internal_stats_ ?
      histogramType < INTERNAL_HISTOGRAM_ENUM_MAX :
      histogramType < HISTOGRAM_ENUM_MAX);
  // Return its own ticker version
  histograms_[histogramType].Data(data);
}

std::string StatisticsImpl::getHistogramString(uint32_t histogramType) const {
  assert(enable_internal_stats_ ? histogramType < INTERNAL_HISTOGRAM_ENUM_MAX
                                : histogramType < HISTOGRAM_ENUM_MAX);
  return histograms_[histogramType].ToString();
}

StatisticsImpl::ThreadTickerInfo* StatisticsImpl::getThreadTickerInfo(
    uint32_t tickerType) {
  auto info_ptr =
      static_cast<ThreadTickerInfo*>(tickers_[tickerType].thread_value->Get());
  if (info_ptr == nullptr) {
    info_ptr =
        new ThreadTickerInfo(0 /* value */, &tickers_[tickerType].merged_sum);
    tickers_[tickerType].thread_value->Reset(info_ptr);
  }
  return info_ptr;
}

void StatisticsImpl::setTickerCount(uint32_t tickerType, uint64_t count) {
  {
    MutexLock lock(&aggregate_lock_);
    assert(enable_internal_stats_ ? tickerType < INTERNAL_TICKER_ENUM_MAX
                                  : tickerType < TICKER_ENUM_MAX);
    if (tickerType < TICKER_ENUM_MAX || enable_internal_stats_) {
      tickers_[tickerType].thread_value->Fold(
          [](void* curr_ptr, void* res) {
            static_cast<std::atomic<uint64_t>*>(curr_ptr)->store(0);
          },
          nullptr /* res */);
      tickers_[tickerType].merged_sum.store(count);
    }
  }
  if (stats_ && tickerType < TICKER_ENUM_MAX) {
    stats_->setTickerCount(tickerType, count);
  }
}

void StatisticsImpl::recordTick(uint32_t tickerType, uint64_t count) {
  assert(
    enable_internal_stats_ ?
      tickerType < INTERNAL_TICKER_ENUM_MAX :
      tickerType < TICKER_ENUM_MAX);
  if (tickerType < TICKER_ENUM_MAX || enable_internal_stats_) {
    auto info_ptr = getThreadTickerInfo(tickerType);
    info_ptr->value.fetch_add(count);
  }
  if (stats_ && tickerType < TICKER_ENUM_MAX) {
    stats_->recordTick(tickerType, count);
  }
}

void StatisticsImpl::measureTime(uint32_t histogramType, uint64_t value) {
  assert(
    enable_internal_stats_ ?
      histogramType < INTERNAL_HISTOGRAM_ENUM_MAX :
      histogramType < HISTOGRAM_ENUM_MAX);
  if (histogramType < HISTOGRAM_ENUM_MAX || enable_internal_stats_) {
    histograms_[histogramType].Add(value);
  }
  if (stats_ && histogramType < HISTOGRAM_ENUM_MAX) {
    stats_->measureTime(histogramType, value);
  }
}

namespace {

// a buffer size used for temp string buffers
const int kBufferSize = 200;

} // namespace

std::string StatisticsImpl::ToString() const {
  std::string res;
  res.reserve(20000);
  for (const auto& t : TickersNameMap) {
    if (t.first < TICKER_ENUM_MAX || enable_internal_stats_) {
      char buffer[kBufferSize];
      snprintf(buffer, kBufferSize, "%s COUNT : %" PRIu64 "\n",
               t.second.c_str(), getTickerCount(t.first));
      res.append(buffer);
    }
  }
  for (const auto& h : HistogramsNameMap) {
    if (h.first < HISTOGRAM_ENUM_MAX || enable_internal_stats_) {
      char buffer[kBufferSize];
      HistogramData hData;
      histogramData(h.first, &hData);
      snprintf(
          buffer,
          kBufferSize,
          "%s statistics Percentiles :=> 50 : %f 95 : %f 99 : %f\n",
          h.second.c_str(),
          hData.median,
          hData.percentile95,
          hData.percentile99);
      res.append(buffer);
    }
  }
  res.shrink_to_fit();
  return res;
}

bool StatisticsImpl::HistEnabledForType(uint32_t type) const {
  if (LIKELY(!enable_internal_stats_)) {
    return type < HISTOGRAM_ENUM_MAX;
  }
  return true;
}

} // namespace rocksdb
