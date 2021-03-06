// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/test/appcache_test_helper.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/message_loop/message_loop.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "webkit/browser/appcache/appcache.h"
#include "webkit/browser/appcache/appcache_entry.h"
#include "webkit/browser/appcache/appcache_group.h"
#include "webkit/browser/appcache/appcache_service.h"

namespace content {

AppCacheTestHelper::AppCacheTestHelper()
    : group_id_(0),
      appcache_id_(0),
      response_id_(0),
      origins_(NULL) {}

AppCacheTestHelper::~AppCacheTestHelper() {}

void AppCacheTestHelper::OnGroupAndNewestCacheStored(
    appcache::AppCacheGroup* /*group*/,
    appcache::AppCache* /*newest_cache*/,
    bool success,
    bool /*would_exceed_quota*/) {
  ASSERT_TRUE(success);
  base::MessageLoop::current()->Quit();
}

void AppCacheTestHelper::AddGroupAndCache(appcache::AppCacheService*
    appcache_service, const GURL& manifest_url) {
  appcache::AppCacheGroup* appcache_group =
      new appcache::AppCacheGroup(appcache_service->storage(),
                                  manifest_url,
                                  ++group_id_);
  appcache::AppCache* appcache = new appcache::AppCache(
      appcache_service->storage(), ++appcache_id_);
  appcache::AppCacheEntry entry(appcache::AppCacheEntry::MANIFEST,
                                ++response_id_);
  appcache->AddEntry(manifest_url, entry);
  appcache->set_complete(true);
  appcache_group->AddCache(appcache);
  appcache_service->storage()->StoreGroupAndNewestCache(appcache_group,
                                                        appcache,
                                                        this);
  // OnGroupAndNewestCacheStored will quit the message loop.
  base::MessageLoop::current()->Run();
}

void AppCacheTestHelper::GetOriginsWithCaches(appcache::AppCacheService*
    appcache_service, std::set<GURL>* origins) {
  appcache_info_ = new appcache::AppCacheInfoCollection;
  origins_ = origins;
  appcache_service->GetAllAppCacheInfo(
      appcache_info_.get(),
      base::Bind(&AppCacheTestHelper::OnGotAppCacheInfo,
                 base::Unretained(this)));

  // OnGotAppCacheInfo will quit the message loop.
  base::MessageLoop::current()->Run();
}

void AppCacheTestHelper::OnGotAppCacheInfo(int rv) {
  typedef std::map<GURL, appcache::AppCacheInfoVector> InfoByOrigin;

  origins_->clear();
  for (InfoByOrigin::const_iterator origin =
           appcache_info_->infos_by_origin.begin();
       origin != appcache_info_->infos_by_origin.end(); ++origin) {
    origins_->insert(origin->first);
  }
  base::MessageLoop::current()->Quit();
}

}  // namespace content
