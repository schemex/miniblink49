// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_DOM_STORAGE_WEBSTORAGEAREA_IMPL_H_
#define CONTENT_RENDERER_DOM_STORAGE_WEBSTORAGEAREA_IMPL_H_

#include "third_party/WebKit/public/platform/WebStorageArea.h"
#include "third_party/WebKit/public/platform/WebString.h"
#include "third_party/WebKit/Source/web/WebStorageNamespaceImpl.h"
#include "wtf/HashMap.h"

namespace blink {

class KURL;
class DOMStorageCachedArea;

class WebStorageAreaImpl : public WebStorageArea {
public:
    WebStorageAreaImpl(DOMStorageMap& cachedArea, const WebString& origin);
    virtual ~WebStorageAreaImpl();

    // See WebStorageArea.h for documentation on these functions.
    virtual unsigned length();

    virtual WebString key(unsigned index);

    virtual WebString getItem(const WebString& key);

    virtual void setItem(const WebString& key, const WebString& value,
        const WebURL& page_url, WebStorageArea::Result& result);

    virtual void removeItem(const WebString& key, const WebURL& page_url);

    virtual void clear(const WebURL& url);

    virtual size_t memoryBytesUsedByCache() const;

private:
    String m_origin;
    DOMStorageMap& m_cachedArea;
};

}  // namespace blink

#endif  // CONTENT_RENDERER_DOM_STORAGE_WEBSTORAGEAREA_IMPL_H_
