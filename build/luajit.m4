dnl -------------------------------------------------------- -*- autoconf -*-
dnl Licensed to the Apache Software Foundation (ASF) under one or more
dnl contributor license agreements.  See the NOTICE file distributed with
dnl this work for additional information regarding copyright ownership.
dnl The ASF licenses this file to You under the Apache License, Version 2.0
dnl (the "License"); you may not use this file except in compliance with
dnl the License.  You may obtain a copy of the License at
dnl
dnl     http://www.apache.org/licenses/LICENSE-2.0
dnl
dnl Unless required by applicable law or agreed to in writing, software
dnl distributed under the License is distributed on an "AS IS" BASIS,
dnl WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
dnl See the License for the specific language governing permissions and
dnl limitations under the License.

dnl
dnl luajit.m4 Trafficserver's LuaJIT autoconf macros
dnl

dnl
dnl TS_CHECK_LUA: look for LuaJIT libraries and headers. Fallback on Lua if LuaJIt not found
dnl
AC_DEFUN([TS_CHECK_LUA], [
  PKG_CHECK_MODULES([LUAJIT], [luajit], [have_luajit=yes], [have_luajit=no])
  PKG_CHECK_MODULES([LUALIB], [lua5.2], [have_lua=yes], [have_lua=no])
  AS_IF(
    [ test "x${have_luajit}" = "xyes" ],
    [
      AC_SUBST([LUA_LIBS],[$LUAJIT_LIBS])
      AC_SUBST([LUA_CFLAGS],[$LUAJIT_CFLAGS])
      AC_SUBST([LUA_CPPFLAGS],[$LUAJIT_CFLAGS])
      AC_SUBST([LUA_LDFLAGS],[$LUAJIT_LDFLAGS])
    ],
    [
      AS_IF(
        [ test "x${have_lua}" = "xyes" ],
        [
          AC_SUBST([LUA_LIBS],[$LUALIB_LIBS])
          AC_SUBST([LUA_CFLAGS],[$LUALIB_CFLAGS])
          AC_SUBST([LUA_CPPFLAGS],[$LUALIB_CFLAGS])
          AC_SUBST([LUA_LDFLAGS],[$LUALIB_LDFLAGS])
        ],
        [ AC_MSG_ERROR([Neither LuaJIT nor Lua package available]) ]
      )
    ]
  )
])
