/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#ifndef _ESI_HTTP_HEADER_H

#define _ESI_HTTP_HEADER_H

#include <list>

namespace EsiLib
{
struct HttpHeader {
  const char *name;
  int name_len;
  const char *value;
  int value_len;
  HttpHeader(const char *n = 0, int n_len = -1, const char *v = 0, int v_len = -1)
    : name(n), name_len(n_len), value(v), value_len(v_len){};
};

typedef std::list<HttpHeader> HttpHeaderList;
};

#endif // _ESI_HTTP_HEADER_H
