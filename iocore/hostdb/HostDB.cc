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

#define _HOSTDB_CC_

#include "P_HostDB.h"
#include "P_RefCountCacheSerializer.h"
#include "ts/I_Layout.h"
#include "Show.h"
#include "ts/Tokenizer.h"

#include <vector>
#include <algorithm>

// dxu: turn off all Diags.h 's function.
//#define Debug
//#define Warning
//#define Note

#include "ts/ink_apidefs.h"

HostDBProcessor hostDBProcessor;
int HostDBProcessor::hostdb_strict_round_robin = 0;
int HostDBProcessor::hostdb_timed_round_robin  = 0;
HostDBProcessor::Options const HostDBProcessor::DEFAULT_OPTIONS;
HostDBContinuation::Options const HostDBContinuation::DEFAULT_OPTIONS;
int hostdb_enable                              = true;
int hostdb_migrate_on_demand                   = true;
int hostdb_cluster                             = false;
int hostdb_cluster_round_robin                 = false;
int hostdb_lookup_timeout                      = 30;
int hostdb_insert_timeout                      = 160;
int hostdb_re_dns_on_reload                    = false;
int hostdb_ttl_mode                            = TTL_OBEY;
unsigned int hostdb_ip_stale_interval          = HOST_DB_IP_STALE;
unsigned int hostdb_ip_timeout_interval        = HOST_DB_IP_TIMEOUT;
unsigned int hostdb_ip_fail_timeout_interval   = HOST_DB_IP_FAIL_TIMEOUT;
unsigned int hostdb_serve_stale_but_revalidate = 0;
unsigned int hostdb_hostfile_check_interval    = 86400; // 1 day
// Epoch timestamp of the current hosts file check.
ink_time_t hostdb_current_interval = 0;
// Epoch timestamp of the last time we actually checked for a hosts file update.
static ink_time_t hostdb_last_interval = 0;
// Epoch timestamp when we updated the hosts file last.
static ink_time_t hostdb_hostfile_update_timestamp = 0;
static char hostdb_filename[PATH_NAME_MAX]         = DEFAULT_HOST_DB_FILENAME;
int hostdb_max_count                               = DEFAULT_HOST_DB_SIZE;
char hostdb_hostfile_path[PATH_NAME_MAX]           = "";
int hostdb_sync_frequency                          = 120;
int hostdb_disable_reverse_lookup                  = 0;

ClassAllocator<HostDBContinuation> hostDBContAllocator("hostDBContAllocator");

// Static configuration information

HostDBCache hostDB;

void ParseHostFile(char const *path, unsigned int interval);

char *
HostDBInfo::srvname(HostDBRoundRobin *rr) const
{
  if (!is_srv || !data.srv.srv_offset)
    return NULL;
  return (char *)rr + data.srv.srv_offset;
}

static inline bool
is_addr_valid(uint8_t af, ///< Address family (format of data)
              void *ptr   ///< Raw address data (not a sockaddr variant!)
              )
{
  return (AF_INET == af && INADDR_ANY != *(reinterpret_cast<in_addr_t *>(ptr))) ||
         (AF_INET6 == af && !IN6_IS_ADDR_UNSPECIFIED(reinterpret_cast<in6_addr *>(ptr)));
}

static inline void
ip_addr_set(sockaddr *ip, ///< Target storage, sockaddr compliant.
            uint8_t af,   ///< Address format.
            void *ptr     ///< Raw address data
            )
{
  if (AF_INET6 == af)
    ats_ip6_set(ip, *static_cast<in6_addr *>(ptr));
  else if (AF_INET == af)
    ats_ip4_set(ip, *static_cast<in_addr_t *>(ptr));
  else
    ats_ip_invalidate(ip);
}

static inline void
ip_addr_set(IpAddr &ip, ///< Target storage.
            uint8_t af, ///< Address format.
            void *ptr   ///< Raw address data
            )
{
  if (AF_INET6 == af)
    ip = *static_cast<in6_addr *>(ptr);
  else if (AF_INET == af)
    ip = *static_cast<in_addr_t *>(ptr);
  else
    ip.invalidate();
}

inline void
hostdb_cont_free(HostDBContinuation *cont)
{
  if (cont->pending_action)
    cont->pending_action->cancel();
  cont->mutex        = 0;
  cont->action.mutex = 0;
  hostDBContAllocator.free(cont);
}

/* Check whether a resolution fail should lead to a retry.
   The @a mark argument is updated if appropriate.
   @return @c true if @a mark was updated, @c false if no retry should be done.
*/
static inline bool
check_for_retry(HostDBMark &mark, HostResStyle style)
{
  bool zret = true;
  if (HOSTDB_MARK_IPV4 == mark && HOST_RES_IPV4 == style)
    mark = HOSTDB_MARK_IPV6;
  else if (HOSTDB_MARK_IPV6 == mark && HOST_RES_IPV6 == style)
    mark = HOSTDB_MARK_IPV4;
  else
    zret = false;
  return zret;
}

char const *
string_for(HostDBMark mark)
{
  static char const *STRING[] = {"Generic", "IPv4", "IPv6", "SRV"};
  return STRING[mark];
}

//
// Function Prototypes
//
static Action *register_ShowHostDB(Continuation *c, HTTPHdr *h);

HostDBMD5 &
HostDBMD5::set_host(char const *name, int len)
{
  host_name = name;
  host_len  = len;
#ifdef SPLIT_DNS
  if (host_name && SplitDNSConfig::isSplitDNSEnabled()) {
    const char *scan;
    // I think this is checking for a hostname that is just an address.
    for (scan = host_name; *scan != '\0' && (ParseRules::is_digit(*scan) || '.' == *scan || ':' == *scan); ++scan)
      ;
    if ('\0' != *scan) {
      // config is released in the destructor, because we must make sure values we
      // get out of it don't evaporate while @a this is still around.
      if (!pSD)
        pSD = SplitDNSConfig::acquire();
      if (pSD) {
        dns_server = static_cast<DNSServer *>(pSD->getDNSRecord(host_name));
      }
    } else {
      dns_server = 0;
    }
  }
#endif // SPLIT_DNS
  return *this;
}

void
HostDBMD5::refresh()
{
  MD5Context ctx;

  if (host_name) {
    char const *server_line = dns_server ? dns_server->x_dns_ip_line : 0;
    uint8_t m               = static_cast<uint8_t>(db_mark); // be sure of the type.

    ctx.update(host_name, host_len);
    ctx.update(reinterpret_cast<uint8_t *>(&port), sizeof(port));
    ctx.update(&m, sizeof(m));
    if (server_line)
      ctx.update(server_line, strlen(server_line));
  } else {
    // INK_MD5 the ip, pad on both sizes with 0's
    // so that it does not intersect the string space
    //
    char buff[TS_IP6_SIZE + 4];
    int n = ip.isIp6() ? sizeof(in6_addr) : sizeof(in_addr_t);
    memset(buff, 0, 2);
    memcpy(buff + 2, ip._addr._byte, n);
    memset(buff + 2 + n, 0, 2);
    ctx.update(buff, n + 4);
  }
  ctx.finalize(hash);
}

HostDBMD5::HostDBMD5() : host_name(0), host_len(0), port(0), dns_server(0), pSD(0), db_mark(HOSTDB_MARK_GENERIC)
{
}

HostDBMD5::~HostDBMD5()
{
  if (pSD)
    SplitDNSConfig::release(pSD);
}

HostDBCache::HostDBCache() : refcountcache(NULL), pending_dns(NULL), remoteHostDBQueue(NULL)
{
  hosts_file_ptr = new RefCountedHostsFileMap();
}

HostDBCache *
HostDBProcessor::cache()
{
  return &hostDB;
}

struct HostDBBackgroundTask : public Continuation {
  int frequency;
  ink_hrtime start_time;

  virtual int sync_event(int event, void *edata) = 0;
  int wait_event(int event, void *edata);

  HostDBBackgroundTask(int frequency);
};

HostDBBackgroundTask::HostDBBackgroundTask(int frequency) : Continuation(new_ProxyMutex()), frequency(frequency), start_time(0)
{
  SET_HANDLER(&HostDBBackgroundTask::sync_event);
}

int
HostDBBackgroundTask::wait_event(int, void *)
{
  ink_hrtime next_sync = HRTIME_SECONDS(this->frequency) - (Thread::get_hrtime() - start_time);

  SET_HANDLER(&HostDBBackgroundTask::sync_event);
  if (next_sync > HRTIME_MSECONDS(100))
    eventProcessor.schedule_in(this, next_sync, ET_TASK);
  else
    eventProcessor.schedule_imm(this, ET_TASK);
  return EVENT_DONE;
}

struct HostDBSync : public HostDBBackgroundTask {
  std::string storage_path;
  std::string full_path;
  HostDBSync(int frequency, std::string storage_path, std::string full_path)
    : HostDBBackgroundTask(frequency), storage_path(storage_path), full_path(full_path){};
  int
  sync_event(int, void *)
  {
    SET_HANDLER(&HostDBSync::wait_event);
    start_time = Thread::get_hrtime();

    new RefCountCacheSerializer<RefCountCache<HostDBInfo>>(this, hostDBProcessor.cache()->refcountcache, this->frequency,
                                                           this->storage_path, this->full_path);
    return EVENT_DONE;
  }
};

int
HostDBCache::start(int flags)
{
  (void)flags; // unused
  char storage_path[PATH_NAME_MAX];
  MgmtInt hostdb_max_size = 0;
  int hostdb_partitions   = 64;

  storage_path[0] = '\0';

  // Read configuration
  // Command line overrides manager configuration.
  //
  REC_ReadConfigInt32(hostdb_enable, "proxy.config.hostdb");
  REC_ReadConfigString(storage_path, "proxy.config.hostdb.storage_path", sizeof(storage_path));
  REC_ReadConfigString(hostdb_filename, "proxy.config.hostdb.filename", sizeof(hostdb_filename));

  // Max number of items
  REC_ReadConfigInt32(hostdb_max_count, "proxy.config.hostdb.max_count");
  // max size allowed to use
  REC_ReadConfigInteger(hostdb_max_size, "proxy.config.hostdb.max_size");
  // number of partitions
  REC_ReadConfigInt32(hostdb_partitions, "proxy.config.hostdb.partitions");
  // how often to sync hostdb to disk
  REC_EstablishStaticConfigInt32(hostdb_sync_frequency, "proxy.config.cache.hostdb.sync_frequency");

  if (hostdb_max_size == 0) {
    Fatal("proxy.config.hostdb.max_size must be a non-zero number");
  }

  // If proxy.config.hostdb.storage_path is not set, use the local state dir. If it is set to
  // a relative path, make it relative to the prefix.
  if (storage_path[0] == '\0') {
    ats_scoped_str rundir(RecConfigReadRuntimeDir());
    ink_strlcpy(storage_path, rundir, sizeof(storage_path));
  } else if (storage_path[0] != '/') {
    Layout::relative_to(storage_path, sizeof(storage_path), Layout::get()->prefix, storage_path);
  }

  Debug("hostdb", "Storage path is %s", storage_path);

  if (access(storage_path, W_OK | R_OK) == -1) {
    Warning("Unable to access() directory '%s': %d, %s", storage_path, errno, strerror(errno));
    Warning("Please set 'proxy.config.hostdb.storage_path' or 'proxy.config.local_state_dir'");
  }

  // Combine the path and name
  char full_path[2 * PATH_NAME_MAX];
  ink_filepath_make(full_path, 2 * PATH_NAME_MAX, storage_path, hostdb_filename);

  Debug("hostdb", "Opening %s, partitions=%d storage_size=%" PRIu64 " items=%d", full_path, hostdb_partitions, hostdb_max_size,
        hostdb_max_count);
  this->refcountcache = new RefCountCache<HostDBInfo>(hostdb_partitions, hostdb_max_size, hostdb_max_count, HostDBInfo::version(),
                                                      "proxy.process.hostdb.cache.");
  int load_ret = LoadRefCountCacheFromPath<HostDBInfo>(*this->refcountcache, storage_path, full_path, HostDBInfo::unmarshall);
  if (load_ret != 0) {
    Warning("Error loading cache from %s: %d", full_path, load_ret);
  }

  //
  // Sync HostDB, if we've asked for it.
  //
  if (hostdb_sync_frequency > 0) {
    eventProcessor.schedule_imm(new HostDBSync(hostdb_sync_frequency, storage_path, full_path), ET_TASK);
  }

  this->pending_dns       = new Queue<HostDBContinuation, Continuation::Link_link>[hostdb_partitions];
  this->remoteHostDBQueue = new Queue<HostDBContinuation, Continuation::Link_link>[hostdb_partitions];
  return 0;
}

// Start up the Host Database processor.
// Load configuration, register configuration and statistics and
// open the cache. This doesn't create any threads, so those
// parameters are ignored.
//
int
HostDBProcessor::start(int, size_t)
{
  if (hostDB.start(0) < 0)
    return -1;

  if (auto_clear_hostdb_flag)
    hostDB.refcountcache->clear();

  statPagesManager.register_http("hostdb", register_ShowHostDB);

  //
  // Register configuration callback, and establish configuation links
  //
  REC_EstablishStaticConfigInt32(hostdb_ttl_mode, "proxy.config.hostdb.ttl_mode");
  REC_EstablishStaticConfigInt32(hostdb_disable_reverse_lookup, "proxy.config.cache.hostdb.disable_reverse_lookup");
  REC_EstablishStaticConfigInt32(hostdb_re_dns_on_reload, "proxy.config.hostdb.re_dns_on_reload");
  REC_EstablishStaticConfigInt32(hostdb_migrate_on_demand, "proxy.config.hostdb.migrate_on_demand");
  REC_EstablishStaticConfigInt32(hostdb_strict_round_robin, "proxy.config.hostdb.strict_round_robin");
  REC_EstablishStaticConfigInt32(hostdb_timed_round_robin, "proxy.config.hostdb.timed_round_robin");
  REC_EstablishStaticConfigInt32(hostdb_cluster, "proxy.config.hostdb.cluster");
  REC_EstablishStaticConfigInt32(hostdb_cluster_round_robin, "proxy.config.hostdb.cluster.round_robin");
  REC_EstablishStaticConfigInt32(hostdb_lookup_timeout, "proxy.config.hostdb.lookup_timeout");
  REC_EstablishStaticConfigInt32U(hostdb_ip_timeout_interval, "proxy.config.hostdb.timeout");
  REC_EstablishStaticConfigInt32U(hostdb_ip_stale_interval, "proxy.config.hostdb.verify_after");
  REC_EstablishStaticConfigInt32U(hostdb_ip_fail_timeout_interval, "proxy.config.hostdb.fail.timeout");
  REC_EstablishStaticConfigInt32U(hostdb_serve_stale_but_revalidate, "proxy.config.hostdb.serve_stale_for");
  REC_EstablishStaticConfigInt32U(hostdb_hostfile_check_interval, "proxy.config.hostdb.host_file.interval");

  //
  // Set up hostdb_current_interval
  //
  hostdb_current_interval = ink_time();

  HostDBContinuation *b = hostDBContAllocator.alloc();
  SET_CONTINUATION_HANDLER(b, (HostDBContHandler)&HostDBContinuation::backgroundEvent);
  b->mutex = new_ProxyMutex();
  eventProcessor.schedule_every(b, HRTIME_SECONDS(1), ET_DNS);

  return 0;
}

void
HostDBContinuation::init(HostDBMD5 const &the_md5, Options const &opt)
{
  md5 = the_md5;
  if (md5.host_name) {
    // copy to backing store.
    if (md5.host_len > static_cast<int>(sizeof(md5_host_name_store) - 1))
      md5.host_len = sizeof(md5_host_name_store) - 1;
    memcpy(md5_host_name_store, md5.host_name, md5.host_len);
  } else {
    md5.host_len = 0;
  }
  md5_host_name_store[md5.host_len] = 0;
  md5.host_name                     = md5_host_name_store;

  host_res_style     = opt.host_res_style;
  dns_lookup_timeout = opt.timeout;
  mutex              = hostDB.refcountcache->lock_for_key(md5.hash.fold());
  if (opt.cont) {
    action = opt.cont;
  } else {
    // ink_assert(!"this sucks");
    action.mutex = mutex;
  }
}

void
HostDBContinuation::refresh_MD5()
{
  ProxyMutex *old_bucket_mutex = hostDB.refcountcache->lock_for_key(md5.hash.fold());
  // We're not pending DNS anymore.
  remove_trigger_pending_dns();
  md5.refresh();
  // Update the mutex if it's from the bucket.
  // Some call sites modify this after calling @c init so need to check.
  if (mutex.get() == old_bucket_mutex) {
    mutex = hostDB.refcountcache->lock_for_key(md5.hash.fold());
  }
}

static bool
reply_to_cont(Continuation *cont, HostDBInfo *r, bool is_srv = false)
{
  if (r == NULL || r->is_srv != is_srv || r->is_failed()) {
    cont->handleEvent(is_srv ? EVENT_SRV_LOOKUP : EVENT_HOST_DB_LOOKUP, NULL);
    return false;
  }

  if (r->reverse_dns) {
    if (!r->hostname()) {
      ink_assert(!"missing hostname");
      cont->handleEvent(is_srv ? EVENT_SRV_LOOKUP : EVENT_HOST_DB_LOOKUP, NULL);
      Warning("bogus entry deleted from HostDB: missing hostname");
      hostDB.refcountcache->erase(r->key);
      return false;
    }
    Debug("hostdb", "hostname = %s", r->hostname());
  }

  if (!r->is_srv && r->round_robin) {
    if (!r->rr()) {
      ink_assert(!"missing round-robin");
      cont->handleEvent(is_srv ? EVENT_SRV_LOOKUP : EVENT_HOST_DB_LOOKUP, NULL);
      Warning("bogus entry deleted from HostDB: missing round-robin");
      hostDB.refcountcache->erase(r->key);
      return false;
    }
    ip_text_buffer ipb;
    Debug("hostdb", "RR of %d with %d good, 1st IP = %s", r->rr()->rrcount, r->rr()->good, ats_ip_ntop(r->ip(), ipb, sizeof ipb));
  }

  cont->handleEvent(is_srv ? EVENT_SRV_LOOKUP : EVENT_HOST_DB_LOOKUP, r);

  return true;
}

inline HostResStyle
host_res_style_for(sockaddr const *ip)
{
  return ats_is_ip6(ip) ? HOST_RES_IPV6_ONLY : HOST_RES_IPV4_ONLY;
}

inline HostResStyle
host_res_style_for(HostDBMark mark)
{
  return HOSTDB_MARK_IPV4 == mark ? HOST_RES_IPV4_ONLY : HOSTDB_MARK_IPV6 == mark ? HOST_RES_IPV6_ONLY : HOST_RES_NONE;
}

inline HostDBMark
db_mark_for(HostResStyle style)
{
  HostDBMark zret = HOSTDB_MARK_GENERIC;
  if (HOST_RES_IPV4 == style || HOST_RES_IPV4_ONLY == style)
    zret = HOSTDB_MARK_IPV4;
  else if (HOST_RES_IPV6 == style || HOST_RES_IPV6_ONLY == style)
    zret = HOSTDB_MARK_IPV6;
  return zret;
}

inline HostDBMark
db_mark_for(sockaddr const *ip)
{
  return ats_is_ip6(ip) ? HOSTDB_MARK_IPV6 : HOSTDB_MARK_IPV4;
}

inline HostDBMark
db_mark_for(IpAddr const &ip)
{
  return ip.isIp6() ? HOSTDB_MARK_IPV6 : HOSTDB_MARK_IPV4;
}

Ptr<HostDBInfo>
probe(ProxyMutex *mutex, HostDBMD5 const &md5, bool ignore_timeout)
{
  // If hostdb is disabled, don't return anything
  if (!hostdb_enable) {
    return Ptr<HostDBInfo>();
  }

  // Otherwise HostDB is enabled, so we'll do our thing
  ink_assert(this_ethread() == hostDB.refcountcache->lock_for_key(md5.hash.fold())->thread_holding);
  uint64_t folded_md5 = md5.hash.fold();

  // get the item from cache
  Ptr<HostDBInfo> r = hostDB.refcountcache->get(folded_md5);
  // If there was nothing in the cache-- this is a miss
  if (r.get() == NULL) {
    return r;
  }

  // If the dns response was failed, and we've hit the failed timeout, lets stop returning it
  if (r->is_failed() && r->is_ip_fail_timeout()) {
    return make_ptr((HostDBInfo *)NULL);
    // if we aren't ignoring timeouts, and we are past it-- then remove the item
  } else if (!ignore_timeout && r->is_ip_timeout() && !r->serve_stale_but_revalidate()) {
    HOSTDB_INCREMENT_DYN_STAT(hostdb_ttl_expires_stat);
    return make_ptr((HostDBInfo *)NULL);
  }

  // If the record is stale, but we want to revalidate-- lets start that up
  if ((!ignore_timeout && r->is_ip_stale() && !cluster_machine_at_depth(master_hash(md5.hash)) && !r->reverse_dns) ||
      (r->is_ip_timeout() && r->serve_stale_but_revalidate())) {
    Debug("hostdb", "stale %u %u %u, using it and refreshing it", r->ip_interval(), r->ip_timestamp, r->ip_timeout_interval);
    HostDBContinuation *c = hostDBContAllocator.alloc();
    HostDBContinuation::Options copt;
    copt.host_res_style = host_res_style_for(r->ip());
    c->init(md5, copt);
    c->do_dns();
  }
  return r;
}

//
// Insert a HostDBInfo into the database
// A null value indicates that the block is empty.
//
HostDBInfo *
HostDBContinuation::insert(unsigned int attl)
{
  uint64_t folded_md5 = md5.hash.fold();

  ink_assert(this_ethread() == hostDB.refcountcache->lock_for_key(folded_md5)->thread_holding);

  HostDBInfo *r = HostDBInfo::alloc();
  r->key        = folded_md5;
  if (attl > HOST_DB_MAX_TTL)
    attl                 = HOST_DB_MAX_TTL;
  r->ip_timeout_interval = attl;
  r->ip_timestamp        = hostdb_current_interval;
  Debug("hostdb", "inserting for: %.*s: (md5: %" PRIx64 ") now: %u timeout: %u ttl: %u", md5.host_len, md5.host_name, folded_md5,
        r->ip_timestamp, r->ip_timeout_interval, attl);

  hostDB.refcountcache->put(folded_md5, r, 0, r->expiry_time());
  return r;
}

//
// Get an entry by either name or IP
//
Action *
HostDBProcessor::getby(Continuation *cont, const char *hostname, int len, sockaddr const *ip, bool aforce_dns,
                       HostResStyle host_res_style, int dns_lookup_timeout)
{
  HostDBMD5 md5;
  EThread *thread   = this_ethread();
  ProxyMutex *mutex = thread->mutex.get();
  ip_text_buffer ipb;

  HOSTDB_INCREMENT_DYN_STAT(hostdb_total_lookups_stat);

  if ((!hostdb_enable || (hostname && !*hostname)) || (hostdb_disable_reverse_lookup && ip)) {
    MUTEX_TRY_LOCK(lock, cont->mutex, thread);
    if (!lock.is_locked())
      goto Lretry;
    cont->handleEvent(EVENT_HOST_DB_LOOKUP, NULL);
    return ACTION_RESULT_DONE;
  }

  // Load the MD5 data.
  md5.set_host(hostname, hostname ? (len ? len : strlen(hostname)) : 0);
  md5.ip.assign(ip);
  md5.port    = ip ? ats_ip_port_host_order(ip) : 0;
  md5.db_mark = db_mark_for(host_res_style);
  md5.refresh();

  // Attempt to find the result in-line, for level 1 hits
  //
  if (!aforce_dns) {
    bool loop;
    do {
      loop = false; // Only loop on explicit set for retry.
      // find the partition lock
      //
      // TODO: Could we reuse the "mutex" above safely? I think so but not sure.
      ProxyMutex *bmutex = hostDB.refcountcache->lock_for_key(md5.hash.fold());
      MUTEX_TRY_LOCK(lock, bmutex, thread);
      MUTEX_TRY_LOCK(lock2, cont->mutex, thread);

      if (lock.is_locked() && lock2.is_locked()) {
        // If we can get the lock and a level 1 probe succeeds, return
        Ptr<HostDBInfo> r = probe(bmutex, md5, aforce_dns);
        if (r) {
          if (r->is_failed() && hostname)
            loop = check_for_retry(md5.db_mark, host_res_style);
          if (!loop) {
            // No retry -> final result. Return it.
            Debug("hostdb", "immediate answer for %s",
                  hostname ? hostname : ats_is_ip(ip) ? ats_ip_ntop(ip, ipb, sizeof ipb) : "<null>");
            HOSTDB_INCREMENT_DYN_STAT(hostdb_total_hits_stat);
            reply_to_cont(cont, r.get());
            return ACTION_RESULT_DONE;
          }
          md5.refresh(); // only on reloop, because we've changed the family.
        }
      }
    } while (loop);
  }
  Debug("hostdb", "delaying force %d answer for %s", aforce_dns,
        hostname ? hostname : ats_is_ip(ip) ? ats_ip_ntop(ip, ipb, sizeof ipb) : "<null>");

Lretry:
  // Otherwise, create a continuation to do a deeper probe in the background
  //
  HostDBContinuation *c = hostDBContAllocator.alloc();
  HostDBContinuation::Options opt;
  opt.timeout        = dns_lookup_timeout;
  opt.force_dns      = aforce_dns;
  opt.cont           = cont;
  opt.host_res_style = host_res_style;
  c->init(md5, opt);
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::probeEvent);

  // Since ProxyMutexPtr has a cast operator, gcc-3.x get upset
  // about ambiguity when doing this comparison, so by reversing
  // the operands, I force it to pick the cast operation /leif.
  if (thread->mutex == cont->mutex) {
    thread->schedule_in(c, MUTEX_RETRY_DELAY);
  } else {
    dnsProcessor.thread->schedule_imm(c);
  }

  return &c->action;
}

// Wrapper from getbyname to getby
//
Action *
HostDBProcessor::getbyname_re(Continuation *cont, const char *ahostname, int len, Options const &opt)
{
  bool force_dns    = false;
  EThread *thread   = this_ethread();
  ProxyMutex *mutex = thread->mutex.get();

  if (opt.flags & HOSTDB_FORCE_DNS_ALWAYS)
    force_dns = true;
  else if (opt.flags & HOSTDB_FORCE_DNS_RELOAD) {
    force_dns = (hostdb_re_dns_on_reload ? true : false);
    if (force_dns)
      HOSTDB_INCREMENT_DYN_STAT(hostdb_re_dns_on_reload_stat);
  }
  return getby(cont, ahostname, len, 0, force_dns, opt.host_res_style, opt.timeout);
}

Action *
HostDBProcessor::getbynameport_re(Continuation *cont, const char *ahostname, int len, Options const &opt)
{
  bool force_dns    = false;
  EThread *thread   = this_ethread();
  ProxyMutex *mutex = thread->mutex.get();

  if (opt.flags & HOSTDB_FORCE_DNS_ALWAYS)
    force_dns = true;
  else if (opt.flags & HOSTDB_FORCE_DNS_RELOAD) {
    force_dns = (hostdb_re_dns_on_reload ? true : false);
    if (force_dns)
      HOSTDB_INCREMENT_DYN_STAT(hostdb_re_dns_on_reload_stat);
  }
  sockaddr sa;
  ats_ip4_set(&sa, INADDR_ANY, htons(opt.port));
  return getby(cont, ahostname, len, &sa, force_dns, opt.host_res_style, opt.timeout);
}

/* Support SRV records */
Action *
HostDBProcessor::getSRVbyname_imm(Continuation *cont, process_srv_info_pfn process_srv_info, const char *hostname, int len,
                                  Options const &opt)
{
  ink_assert(cont->mutex->thread_holding == this_ethread());
  bool force_dns    = false;
  EThread *thread   = cont->mutex->thread_holding;
  ProxyMutex *mutex = thread->mutex.get();

  if (opt.flags & HOSTDB_FORCE_DNS_ALWAYS)
    force_dns = true;
  else if (opt.flags & HOSTDB_FORCE_DNS_RELOAD) {
    force_dns = (hostdb_re_dns_on_reload ? true : false);
    if (force_dns)
      HOSTDB_INCREMENT_DYN_STAT(hostdb_re_dns_on_reload_stat);
  }

  HostDBMD5 md5;

  HOSTDB_INCREMENT_DYN_STAT(hostdb_total_lookups_stat);

  if (!hostdb_enable || !*hostname) {
    (cont->*process_srv_info)(NULL);
    return ACTION_RESULT_DONE;
  }

  md5.host_name = hostname;
  md5.host_len  = hostname ? (len ? len : strlen(hostname)) : 0;
  md5.port      = 0;
  md5.db_mark   = HOSTDB_MARK_SRV;
  md5.refresh();

  // Attempt to find the result in-line, for level 1 hits
  if (!force_dns) {
    // find the partition lock
    ProxyMutex *bucket_mutex = hostDB.refcountcache->lock_for_key(md5.hash.fold());
    MUTEX_TRY_LOCK(lock, bucket_mutex, thread);

    // If we can get the lock and a level 1 probe succeeds, return
    if (lock.is_locked()) {
      Ptr<HostDBInfo> r = probe(bucket_mutex, md5, false);
      if (r) {
        Debug("hostdb", "immediate SRV answer for %s from hostdb", hostname);
        Debug("dns_srv", "immediate SRV answer for %s from hostdb", hostname);
        HOSTDB_INCREMENT_DYN_STAT(hostdb_total_hits_stat);
        (cont->*process_srv_info)(r.get());
        return ACTION_RESULT_DONE;
      }
    }
  }

  Debug("dns_srv", "delaying (force=%d) SRV answer for %.*s [timeout = %d]", force_dns, md5.host_len, md5.host_name, opt.timeout);

  // Otherwise, create a continuation to do a deeper probe in the background
  HostDBContinuation *c = hostDBContAllocator.alloc();
  HostDBContinuation::Options copt;
  copt.timeout   = opt.timeout;
  copt.cont      = cont;
  copt.force_dns = force_dns;
  c->init(md5, copt);
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::probeEvent);

  if (thread->mutex == cont->mutex) {
    thread->schedule_in(c, MUTEX_RETRY_DELAY);
  } else {
    dnsProcessor.thread->schedule_imm(c);
  }

  return &c->action;
}

// Wrapper from getbyname to getby
//
Action *
HostDBProcessor::getbyname_imm(Continuation *cont, process_hostdb_info_pfn process_hostdb_info, const char *hostname, int len,
                               Options const &opt)
{
  ink_assert(cont->mutex->thread_holding == this_ethread());
  bool force_dns    = false;
  EThread *thread   = cont->mutex->thread_holding;
  ProxyMutex *mutex = thread->mutex.get();
  HostDBMD5 md5;

  if (opt.flags & HOSTDB_FORCE_DNS_ALWAYS)
    force_dns = true;
  else if (opt.flags & HOSTDB_FORCE_DNS_RELOAD) {
    force_dns = (hostdb_re_dns_on_reload ? true : false);
    if (force_dns)
      HOSTDB_INCREMENT_DYN_STAT(hostdb_re_dns_on_reload_stat);
  }

  HOSTDB_INCREMENT_DYN_STAT(hostdb_total_lookups_stat);

  if (!hostdb_enable || !*hostname) {
    (cont->*process_hostdb_info)(NULL);
    return ACTION_RESULT_DONE;
  }

  md5.set_host(hostname, hostname ? (len ? len : strlen(hostname)) : 0);
  md5.port    = opt.port;
  md5.db_mark = db_mark_for(opt.host_res_style);
  md5.refresh();

  // Attempt to find the result in-line, for level 1 hits
  if (!force_dns) {
    bool loop;
    do {
      loop = false; // loop only on explicit set for retry
      // find the partition lock
      ProxyMutex *bucket_mutex = hostDB.refcountcache->lock_for_key(md5.hash.fold());
      SCOPED_MUTEX_LOCK(lock, bucket_mutex, thread);
      // do a level 1 probe for immediate result.
      Ptr<HostDBInfo> r = probe(bucket_mutex, md5, false);
      if (r) {
        if (r->is_failed()) // fail, see if we should retry with alternate
          loop = check_for_retry(md5.db_mark, opt.host_res_style);
        if (!loop) {
          // No retry -> final result. Return it.
          Debug("hostdb", "immediate answer for %.*s", md5.host_len, md5.host_name);
          HOSTDB_INCREMENT_DYN_STAT(hostdb_total_hits_stat);
          (cont->*process_hostdb_info)(r.get());
          return ACTION_RESULT_DONE;
        }
        md5.refresh(); // Update for retry.
      }
    } while (loop);
  }

  Debug("hostdb", "delaying force %d answer for %.*s [timeout %d]", force_dns, md5.host_len, md5.host_name, opt.timeout);

  // Otherwise, create a continuation to do a deeper probe in the background
  HostDBContinuation *c = hostDBContAllocator.alloc();
  HostDBContinuation::Options copt;
  copt.cont           = cont;
  copt.force_dns      = force_dns;
  copt.timeout        = opt.timeout;
  copt.host_res_style = opt.host_res_style;
  c->init(md5, copt);
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::probeEvent);

  thread->schedule_in(c, HOST_DB_RETRY_PERIOD);

  return &c->action;
}

Action *
HostDBProcessor::iterate(Continuation *cont)
{
  ink_assert(cont->mutex->thread_holding == this_ethread());
  EThread *thread   = cont->mutex->thread_holding;
  ProxyMutex *mutex = thread->mutex.get();

  HOSTDB_INCREMENT_DYN_STAT(hostdb_total_lookups_stat);

  HostDBContinuation *c = hostDBContAllocator.alloc();
  HostDBContinuation::Options copt;
  copt.cont           = cont;
  copt.force_dns      = false;
  copt.timeout        = 0;
  copt.host_res_style = HOST_RES_NONE;
  c->init(HostDBMD5(), copt);
  c->current_iterate_pos = 0;
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::iterateEvent);

  if (thread->mutex == cont->mutex) {
    thread->schedule_in(c, HOST_DB_RETRY_PERIOD);
  } else {
    dnsProcessor.thread->schedule_imm(c);
  }

  return &c->action;
}

static void
do_setby(HostDBInfo *r, HostDBApplicationInfo *app, const char *hostname, IpAddr const &ip, bool is_srv = false)
{
  HostDBRoundRobin *rr = r->rr();

  if (is_srv && (!r->is_srv || !rr))
    return;

  if (rr) {
    if (is_srv) {
      uint32_t key = makeHostHash(hostname);
      for (int i = 0; i < rr->rrcount; i++) {
        if (key == rr->info(i).data.srv.key && !strcmp(hostname, rr->info(i).srvname(rr))) {
          Debug("hostdb", "immediate setby for %s", hostname);
          rr->info(i).app.allotment.application1 = app->allotment.application1;
          rr->info(i).app.allotment.application2 = app->allotment.application2;
          return;
        }
      }
    } else
      for (int i = 0; i < rr->rrcount; i++) {
        if (rr->info(i).ip() == ip) {
          Debug("hostdb", "immediate setby for %s", hostname ? hostname : "<addr>");
          rr->info(i).app.allotment.application1 = app->allotment.application1;
          rr->info(i).app.allotment.application2 = app->allotment.application2;
          return;
        }
      }
  } else {
    if (r->reverse_dns || (!r->round_robin && ip == r->ip())) {
      Debug("hostdb", "immediate setby for %s", hostname ? hostname : "<addr>");
      r->app.allotment.application1 = app->allotment.application1;
      r->app.allotment.application2 = app->allotment.application2;
    }
  }
}

void
HostDBProcessor::setby(const char *hostname, int len, sockaddr const *ip, HostDBApplicationInfo *app)
{
  if (!hostdb_enable)
    return;

  HostDBMD5 md5;
  md5.set_host(hostname, hostname ? (len ? len : strlen(hostname)) : 0);
  md5.ip.assign(ip);
  md5.port    = ip ? ats_ip_port_host_order(ip) : 0;
  md5.db_mark = db_mark_for(ip);
  md5.refresh();

  // Attempt to find the result in-line, for level 1 hits

  ProxyMutex *mutex = hostDB.refcountcache->lock_for_key(md5.hash.fold());
  EThread *thread   = this_ethread();
  MUTEX_TRY_LOCK(lock, mutex, thread);

  if (lock.is_locked()) {
    Ptr<HostDBInfo> r = probe(mutex, md5, false);
    if (r)
      do_setby(r.get(), app, hostname, md5.ip);
    return;
  }
  // Create a continuation to do a deaper probe in the background

  HostDBContinuation *c = hostDBContAllocator.alloc();
  c->init(md5);
  c->app.allotment.application1 = app->allotment.application1;
  c->app.allotment.application2 = app->allotment.application2;
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::setbyEvent);
  thread->schedule_in(c, MUTEX_RETRY_DELAY);
}

void
HostDBProcessor::setby_srv(const char *hostname, int len, const char *target, HostDBApplicationInfo *app)
{
  if (!hostdb_enable || !hostname || !target)
    return;

  HostDBMD5 md5;
  md5.set_host(hostname, len ? len : strlen(hostname));
  md5.port    = 0;
  md5.db_mark = HOSTDB_MARK_SRV;
  md5.refresh();

  // Create a continuation to do a deaper probe in the background

  HostDBContinuation *c = hostDBContAllocator.alloc();
  c->init(md5);
  ink_strlcpy(c->srv_target_name, target, MAXDNAME);
  c->app.allotment.application1 = app->allotment.application1;
  c->app.allotment.application2 = app->allotment.application2;
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::setbyEvent);
  eventProcessor.schedule_imm(c);
}
int
HostDBContinuation::setbyEvent(int /* event ATS_UNUSED */, Event * /* e ATS_UNUSED */)
{
  Ptr<HostDBInfo> r = probe(mutex.get(), md5, false);

  if (r)
    do_setby(r.get(), &app, md5.host_name, md5.ip, is_srv());

  hostdb_cont_free(this);
  return EVENT_DONE;
}

static bool
remove_round_robin(HostDBInfo *r, const char *hostname, IpAddr const &ip)
{
  if (r) {
    if (!r->round_robin)
      return false;
    HostDBRoundRobin *rr = r->rr();
    if (!rr)
      return false;
    for (int i = 0; i < rr->good; i++) {
      if (ip == rr->info(i).ip()) {
        ip_text_buffer b;
        Debug("hostdb", "Deleting %s from '%s' round robin DNS entry", ip.toString(b, sizeof b), hostname);
        HostDBInfo tmp         = rr->info(i);
        rr->info(i)            = rr->info(rr->good - 1);
        rr->info(rr->good - 1) = tmp;
        rr->good--;
        if (rr->good <= 0) {
          hostDB.refcountcache->erase(r->key);
          return false;
        } else {
          if (is_debug_tag_set("hostdb")) {
            int bufsize      = rr->good * INET6_ADDRSTRLEN;
            char *rr_ip_list = (char *)alloca(bufsize);
            char *p          = rr_ip_list;
            for (int n = 0; n < rr->good; ++n) {
              ats_ip_ntop(rr->info(n).ip(), p, bufsize);
              int nbytes = strlen(p);
              p += nbytes;
              bufsize -= nbytes;
            }
            Note("'%s' round robin DNS entry updated, entries=%d, IP list: %s", hostname, rr->good, rr_ip_list);
          }
        }
        return true;
      }
    }
  }
  return false;
}

int
HostDBContinuation::removeEvent(int /* event ATS_UNUSED */, Event *e)
{
  Continuation *cont = action.continuation;

  MUTEX_TRY_LOCK(lock, cont ? cont->mutex.get() : (ProxyMutex *)NULL, e->ethread);
  if (!lock.is_locked()) {
    e->schedule_in(HOST_DB_RETRY_PERIOD);
    return EVENT_CONT;
  }
  if (!action.cancelled) {
    if (!hostdb_enable) {
      if (cont)
        cont->handleEvent(EVENT_HOST_DB_IP_REMOVED, (void *)NULL);
    } else {
      Ptr<HostDBInfo> r = probe(mutex.get(), md5, false);
      bool res          = remove_round_robin(r.get(), md5.host_name, md5.ip);
      if (cont)
        cont->handleEvent(EVENT_HOST_DB_IP_REMOVED, res ? static_cast<void *>(&md5.ip) : static_cast<void *>(NULL));
    }
  }
  hostdb_cont_free(this);
  return EVENT_DONE;
}

// Lookup done, insert into the local table, return data to the
// calling continuation or to the calling cluster node.
// NOTE: if "i" exists it means we already allocated the space etc, just return
//
HostDBInfo *
HostDBContinuation::lookup_done(IpAddr const &ip, char const *aname, bool around_robin, unsigned int ttl_seconds, SRVHosts *srv,
                                HostDBInfo *r)
{
  ink_assert(this_ethread() == hostDB.refcountcache->lock_for_key(md5.hash.fold())->thread_holding);
  if (!ip.isValid() || !aname || !aname[0]) {
    if (is_byname()) {
      Debug("hostdb", "lookup_done() failed for '%.*s'", md5.host_len, md5.host_name);
    } else if (is_srv()) {
      Debug("dns_srv", "SRV failed for '%.*s'", md5.host_len, md5.host_name);
    } else {
      ip_text_buffer b;
      Debug("hostdb", "failed for %s", md5.ip.toString(b, sizeof b));
    }
    if (r == NULL) {
      r = insert(hostdb_ip_fail_timeout_interval); // currently ... 0
    } else {
      ttl_seconds = hostdb_ip_fail_timeout_interval;
    }
    r->round_robin     = false;
    r->round_robin_elt = false;
    r->is_srv          = is_srv();
    r->reverse_dns     = !is_byname() && !is_srv();

    r->set_failed();
  } else {
    switch (hostdb_ttl_mode) {
    default:
      ink_assert(!"bad TTL mode");
    case TTL_OBEY:
      break;
    case TTL_IGNORE:
      ttl_seconds = hostdb_ip_timeout_interval;
      break;
    case TTL_MIN:
      if (hostdb_ip_timeout_interval < ttl_seconds) {
        ttl_seconds = hostdb_ip_timeout_interval;
      }
      break;
    case TTL_MAX:
      if (hostdb_ip_timeout_interval > ttl_seconds) {
        ttl_seconds = hostdb_ip_timeout_interval;
      }
      break;
    }
    HOSTDB_SUM_DYN_STAT(hostdb_ttl_stat, ttl_seconds);

    // Not sure about this - it seems wrong but I can't be sure. If we got a fail
    // in the DNS event, 0 is passed in which we then change to 1 here. Do we need this
    // to be non-zero to avoid an infinite timeout?
    if (0 == ttl_seconds)
      ttl_seconds = 1;

    if (r == NULL) {
      r = insert(hostdb_ip_fail_timeout_interval); // currently ... 0
    }
    r->round_robin_elt = false; // only true for elements explicitly added as RR elements.
    if (is_byname()) {
      ip_text_buffer b;
      Debug("hostdb", "done %s TTL %d", ip.toString(b, sizeof b), ttl_seconds);
      ats_ip_set(r->ip(), ip);
      r->round_robin = around_robin;
      r->reverse_dns = false;
      if (md5.host_name != aname) {
        ink_strlcpy(md5_host_name_store, aname, sizeof(md5_host_name_store));
      }
      r->is_srv = false;
    } else if (is_srv()) {
      ink_assert(srv && srv->srv_host_count > 0 && srv->srv_host_count <= 16 && around_robin);

      r->data.srv.srv_offset = srv->srv_host_count;
      r->reverse_dns         = false;
      r->is_srv              = true;
      r->round_robin         = around_robin;

      if (md5.host_name != aname) {
        ink_strlcpy(md5_host_name_store, aname, sizeof(md5_host_name_store));
      }

    } else {
      Debug("hostdb", "done '%s' TTL %d", aname, ttl_seconds);
      // TODO: check that this is right, it seems that the 2 hostnames are always the same
      r->data.hostname_offset = r->hostname_offset;
      // TODO: consolidate into a single "item type" field?
      r->round_robin = false;
      r->reverse_dns = true;
      r->is_srv      = false;
    }
  }

  // Finally, set the TTL
  r->ip_timeout_interval = ttl_seconds;
  // set the "lookup_done" interval
  r->ip_timestamp = hostdb_current_interval;

  if (from_cont)
    do_put_response(from, r, from_cont);
  ink_assert(!r->round_robin || !r->reverse_dns);
  return r;
}

int
HostDBContinuation::dnsPendingEvent(int event, Event *e)
{
  ink_assert(this_ethread() == hostDB.refcountcache->lock_for_key(md5.hash.fold())->thread_holding);
  if (timeout) {
    timeout->cancel(this);
    timeout = NULL;
  }
  if (event == EVENT_INTERVAL) {
    // we timed out, return a failure to the user
    MUTEX_TRY_LOCK_FOR(lock, action.mutex, ((Event *)e)->ethread, action.continuation);
    if (!lock.is_locked()) {
      timeout = eventProcessor.schedule_in(this, HOST_DB_RETRY_PERIOD);
      return EVENT_CONT;
    }
    if (!action.cancelled && action.continuation)
      action.continuation->handleEvent(EVENT_HOST_DB_LOOKUP, NULL);
    hostDB.pending_dns_for_hash(md5.hash).remove(this);
    hostdb_cont_free(this);
    return EVENT_DONE;
  } else {
    SET_HANDLER((HostDBContHandler)&HostDBContinuation::probeEvent);
    return probeEvent(EVENT_INTERVAL, NULL);
  }
}

// for a new HostDBInfo `r`, "inherit" from the old version of yourself if it exists in `old_rr_data`
static int
restore_info(HostDBInfo *r, HostDBInfo *old_r, HostDBInfo &old_info, HostDBRoundRobin *old_rr_data)
{
  if (old_rr_data) {
    for (int j = 0; j < old_rr_data->rrcount; j++)
      if (ats_ip_addr_eq(old_rr_data->info(j).ip(), r->ip())) {
        r->app = old_rr_data->info(j).app;
        return true;
      }
  } else if (old_r)
    if (ats_ip_addr_eq(old_info.ip(), r->ip())) {
      r->app = old_info.app;
      return true;
    }
  return false;
}

// DNS lookup result state
//
int
HostDBContinuation::dnsEvent(int event, HostEnt *e)
{
  ink_assert(this_ethread() == hostDB.refcountcache->lock_for_key(md5.hash.fold())->thread_holding);
  if (timeout) {
    timeout->cancel(this);
    timeout = NULL;
  }
  EThread *thread = mutex->thread_holding;
  if (event == EVENT_INTERVAL) {
    if (!action.continuation) {
      // give up on insert, it has been too long
      remove_trigger_pending_dns();
      hostdb_cont_free(this);
      return EVENT_DONE;
    }
    MUTEX_TRY_LOCK_FOR(lock, action.mutex, thread, action.continuation);
    if (!lock.is_locked()) {
      timeout = thread->schedule_in(this, HOST_DB_RETRY_PERIOD);
      return EVENT_CONT;
    }
    // [amc] Callback to client to indicate a failure due to timeout.
    // We don't try a different family here because a timeout indicates
    // a server issue that won't be fixed by asking for a different
    // address family.
    if (!action.cancelled && action.continuation)
      action.continuation->handleEvent(EVENT_HOST_DB_LOOKUP, NULL);
    action = NULL;
    // do not exit yet, wait to see if we can insert into DB
    timeout = thread->schedule_in(this, HRTIME_SECONDS(hostdb_insert_timeout));
    return EVENT_DONE;
  } else {
    bool failed = !e;

    bool is_rr     = false;
    pending_action = NULL;

    if (is_srv()) {
      is_rr = !failed && (e->srv_hosts.srv_host_count > 0);
    } else if (!failed) {
      is_rr = 0 != e->ent.h_addr_list[1];
    } else {
    }

    ttl             = failed ? 0 : e->ttl / 60;
    int ttl_seconds = failed ? 0 : e->ttl; // ebalsa: moving to second accuracy

    Ptr<HostDBInfo> old_r = probe(mutex.get(), md5, false);
    HostDBInfo old_info;
    if (old_r)
      old_info                    = *old_r.get();
    HostDBRoundRobin *old_rr_data = old_r ? old_r->rr() : NULL;
    int valid_records             = 0;
    void *first_record            = 0;
    uint8_t af                    = e ? e->ent.h_addrtype : AF_UNSPEC; // address family
    // if this is an RR response, we need to find the first record, as well as the
    // total number of records
    if (is_rr) {
      if (is_srv() && !failed) {
        valid_records = e->srv_hosts.srv_host_count;
      } else {
        void *ptr; // tmp for current entry.
        for (int total_records = 0; total_records < HOST_DB_MAX_ROUND_ROBIN_INFO && 0 != (ptr = e->ent.h_addr_list[total_records]);
             ++total_records) {
          if (is_addr_valid(af, ptr)) {
            if (!first_record) {
              first_record = ptr;
            }
            // If we have found some records which are invalid, lets just shuffle around them.
            // This way we'll end up with e->ent.h_addr_list with all the valid responses at
            // the first `valid_records` slots
            if (valid_records != total_records) {
              e->ent.h_addr_list[valid_records] = e->ent.h_addr_list[total_records];
            }

            ++valid_records;
          } else {
            Warning("Zero address removed from round-robin list for '%s'", md5.host_name);
          }
        }
        if (!first_record) {
          failed = true;
          is_rr  = false;
        }
      }
    } else if (!failed) {
      first_record = e->ent.h_addr_list[0];
    } // else first is 0.

    IpAddr tip; // temp storage if needed.

    // In the event that the lookup failed (SOA response-- for example) we want to use md5.host_name, since it'll be ""
    const char *aname = (failed || strlen(md5.host_name)) ? md5.host_name : e->ent.h_name;

    const size_t s_size = strlen(aname) + 1;
    const size_t rrsize = is_rr ? HostDBRoundRobin::size(valid_records, e->srv_hosts.srv_hosts_length) : 0;
    // where in our block of memory we are
    int offset = sizeof(HostDBInfo);

    int allocSize = s_size + rrsize; // The extra space we need for the rest of the things

    Debug("hostdb", "allocating %d bytes for %s with %d RR records", allocSize, aname, valid_records);
    HostDBInfo *r = HostDBInfo::alloc(allocSize);
    // set up the record
    r->key = md5.hash.fold(); // always set the key

    r->hostname_offset = offset;
    ink_strlcpy(r->perm_hostname(), aname, s_size);
    offset += s_size;

    // If the DNS lookup failed (errors such as NXDOMAIN, SERVFAIL, etc.) but we have an old record
    // which is okay with being served stale-- lets continue to serve the stale record as long as
    // the record is willing to be served.
    if (failed && old_r && old_r->serve_stale_but_revalidate()) {
      r = old_r.get();
    } else if (is_byname()) {
      if (first_record)
        ip_addr_set(tip, af, first_record);
      r = lookup_done(tip, md5.host_name, is_rr, ttl_seconds, failed ? 0 : &e->srv_hosts, r);
    } else if (is_srv()) {
      if (!failed)
        tip._family = AF_INET;         // force the tip valid, or else the srv will fail
      r             = lookup_done(tip, /* junk: FIXME: is the code in lookup_done() wrong to NEED this? */
                      md5.host_name,   /* hostname */
                      is_rr,           /* is round robin, doesnt matter for SRV since we recheck getCount() inside lookup_done() */
                      ttl_seconds,     /* ttl in seconds */
                      failed ? 0 : &e->srv_hosts, r);
    } else if (failed) {
      r = lookup_done(tip, md5.host_name, false, ttl_seconds, 0, r);
    } else {
      r = lookup_done(md5.ip, e->ent.h_name, false, ttl_seconds, &e->srv_hosts, r);
    }

    // Conditionally make rr record entries
    if (is_rr) {
      r->app.rr.offset = offset;
      // This will only be set if is_rr
      HostDBRoundRobin *rr_data = (HostDBRoundRobin *)(r->rr());
      ;
      if (is_srv()) {
        int skip  = 0;
        char *pos = (char *)rr_data + sizeof(HostDBRoundRobin) + valid_records * sizeof(HostDBInfo);
        SRV *q[HOST_DB_MAX_ROUND_ROBIN_INFO];
        ink_assert(valid_records <= HOST_DB_MAX_ROUND_ROBIN_INFO);
        // sort
        for (int i = 0; i < valid_records; ++i) {
          q[i] = &e->srv_hosts.hosts[i];
        }
        for (int i = 0; i < valid_records; ++i) {
          for (int ii = i + 1; ii < valid_records; ++ii) {
            if (*q[ii] < *q[i]) {
              SRV *tmp = q[i];
              q[i]     = q[ii];
              q[ii]    = tmp;
            }
          }
        }

        rr_data->good = rr_data->rrcount = valid_records;
        rr_data->current                 = 0;
        for (int i = 0; i < valid_records; ++i) {
          SRV *t                     = q[i];
          HostDBInfo &item           = rr_data->info(i);
          item.round_robin           = 0;
          item.round_robin_elt       = 1;
          item.reverse_dns           = 0;
          item.is_srv                = 1;
          item.data.srv.srv_weight   = t->weight;
          item.data.srv.srv_priority = t->priority;
          item.data.srv.srv_port     = t->port;
          item.data.srv.key          = t->key;

          ink_assert((skip + t->host_len) <= e->srv_hosts.srv_hosts_length);

          memcpy(pos + skip, t->host, t->host_len);
          item.data.srv.srv_offset = (pos - (char *)rr_data) + skip;

          skip += t->host_len;

          item.app.allotment.application1 = 0;
          item.app.allotment.application2 = 0;
          Debug("dns_srv", "inserted SRV RR record [%s] into HostDB with TTL: %d seconds", t->host, ttl_seconds);
        }

        // restore
        if (old_rr_data) {
          for (int i = 0; i < rr_data->rrcount; ++i) {
            for (int ii = 0; ii < old_rr_data->rrcount; ++ii) {
              if (rr_data->info(i).data.srv.key == old_rr_data->info(ii).data.srv.key) {
                char *new_host = rr_data->info(i).srvname(rr_data);
                char *old_host = old_rr_data->info(ii).srvname(old_rr_data);
                if (!strcmp(new_host, old_host))
                  rr_data->info(i).app = old_rr_data->info(ii).app;
              }
            }
          }
        }
      } else { // Otherwise this is a regular dns response
        rr_data->good = rr_data->rrcount = valid_records;
        rr_data->current                 = 0;
        for (int i = 0; i < valid_records; ++i) {
          HostDBInfo &item = rr_data->info(i);
          ip_addr_set(item.ip(), af, e->ent.h_addr_list[i]);
          item.round_robin     = 0;
          item.round_robin_elt = 1;
          item.reverse_dns     = 0;
          item.is_srv          = 0;
          if (!restore_info(&item, old_r.get(), old_info, old_rr_data)) {
            item.app.allotment.application1 = 0;
            item.app.allotment.application2 = 0;
          }
        }
      }
    }

    if (!failed && !is_rr && !is_srv())
      restore_info(r, old_r.get(), old_info, old_rr_data);
    ink_assert(!r || !r->round_robin || !r->reverse_dns);
    ink_assert(failed || !r->round_robin || r->app.rr.offset);

    hostDB.refcountcache->put(md5.hash.fold(), r, allocSize, r->expiry_time());

    // if we are not the owner, put on the owner
    //
    ClusterMachine *m = cluster_machine_at_depth(master_hash(md5.hash));

    if (m)
      do_put_response(m, r, NULL);

    // try to callback the user
    //
    if (action.continuation) {
      // Check for IP family failover
      if (failed && check_for_retry(md5.db_mark, host_res_style)) {
        this->refresh_MD5(); // family changed if we're doing a retry.
        SET_CONTINUATION_HANDLER(this, (HostDBContHandler)&HostDBContinuation::probeEvent);
        thread->schedule_in(this, MUTEX_RETRY_DELAY);
        return EVENT_CONT;
      }

      MUTEX_TRY_LOCK_FOR(lock, action.mutex, thread, action.continuation);
      if (!lock.is_locked()) {
        remove_trigger_pending_dns();
        SET_HANDLER((HostDBContHandler)&HostDBContinuation::probeEvent);
        thread->schedule_in(this, HOST_DB_RETRY_PERIOD);
        return EVENT_CONT;
      }
      if (!action.cancelled)
        reply_to_cont(action.continuation, r, is_srv());
    }
    // wake up everyone else who is waiting
    remove_trigger_pending_dns();

    // all done
    //
    hostdb_cont_free(this);
    return EVENT_DONE;
  }
}

//
// HostDB Get Message
// Used to lookup host information on a remote node in the cluster
//
struct HostDB_get_message {
  INK_MD5 md5;
  IpEndpoint ip;
  Continuation *cont;
  int namelen;
  char name[MAXDNAME];
};

//
// Make a get message
//
int
HostDBContinuation::make_get_message(char *buf, int size)
{
  ink_assert(size >= (int)sizeof(HostDB_get_message));

  HostDB_get_message *msg = reinterpret_cast<HostDB_get_message *>(buf);
  msg->md5                = md5.hash;
  ats_ip_set(&msg->ip.sa, md5.ip, htons(md5.port));
  msg->cont = this;

  // name
  ink_strlcpy(msg->name, md5.host_name, sizeof(msg->name));

  // length
  int len = sizeof(HostDB_get_message) - MAXDNAME + md5.host_len + 1;

  return len;
}

//
// Make and send a get message
//
bool
HostDBContinuation::do_get_response(Event * /* e ATS_UNUSED */)
{
  if (!hostdb_cluster)
    return false;

  // find an appropriate Machine
  //
  ClusterMachine *m = NULL;

  if (hostdb_migrate_on_demand) {
    m = cluster_machine_at_depth(master_hash(md5.hash), &probe_depth, past_probes);
  } else {
    if (probe_depth)
      return false;
    m           = cluster_machine_at_depth(master_hash(md5.hash));
    probe_depth = 1;
  }

  if (!m)
    return false;

  // Make message
  //
  HostDB_get_message msg;

  memset(&msg, 0, sizeof(msg));
  int len = make_get_message((char *)&msg, sizeof(HostDB_get_message));

  // Setup this continuation, with a timeout
  //
  hostDB.remoteHostDBQueue[key_partition()].enqueue(this);
  SET_HANDLER((HostDBContHandler)&HostDBContinuation::clusterEvent);
  timeout = mutex->thread_holding->schedule_in(this, HOST_DB_CLUSTER_TIMEOUT);

  // Send the message
  //
  clusterProcessor.invoke_remote(m->pop_ClusterHandler(), GET_HOSTINFO_CLUSTER_FUNCTION, (char *)&msg, len);

  return true;
}

//
// HostDB Put Message
// This message is used in a response to a cluster node for
// Host inforamation.
//
struct HostDB_put_message {
  INK_MD5 md5;
  IpEndpoint ip;
  unsigned int ttl;
  unsigned int missing : 1;
  unsigned int round_robin : 1;
  Continuation *cont;
  unsigned int application1;
  unsigned int application2;
  int namelen;
  char name[MAXDNAME];
};

//
// Build the put message
//
int
HostDBContinuation::make_put_message(HostDBInfo *r, Continuation *c, char *buf, int size)
{
  ink_assert(size >= (int)sizeof(HostDB_put_message));

  HostDB_put_message *msg = reinterpret_cast<HostDB_put_message *>(buf);
  memset(msg, 0, sizeof(HostDB_put_message));

  msg->md5  = md5.hash;
  msg->cont = c;
  if (r) {
    ats_ip_copy(&msg->ip.sa, r->ip());
    msg->application1 = r->app.allotment.application1;
    msg->application2 = r->app.allotment.application2;
    msg->missing      = false;
    msg->round_robin  = r->round_robin;
    msg->ttl          = r->ip_time_remaining();
  } else {
    msg->missing = true;
  }

  // name
  ink_strlcpy(msg->name, md5.host_name, sizeof(msg->name));

  // length
  int len = sizeof(HostDB_put_message) - MAXDNAME + md5.host_len + 1;

  return len;
}

int
HostDBContinuation::iterateEvent(int event, Event *e)
{
  Debug("hostdb", "iterateEvent event=%d eventp=%p", event, e);
  ink_assert(!link.prev && !link.next);
  EThread *t = e ? e->ethread : this_ethread();

  MUTEX_TRY_LOCK_FOR(lock, action.mutex, t, action.continuation);
  if (!lock.is_locked()) {
    Debug("hostdb", "iterateEvent event=%d eventp=%p: reschedule due to not getting action mutex", event, e);
    mutex->thread_holding->schedule_in(this, HOST_DB_RETRY_PERIOD);
    return EVENT_CONT;
  }

  if (action.cancelled) {
    hostdb_cont_free(this);
    return EVENT_DONE;
  }

  // let's iterate through another record and then reschedule ourself.
  if (current_iterate_pos < hostDB.refcountcache->partition_count()) {
    // TODO: configurable number at a time?
    ProxyMutex *bucket_mutex = hostDB.refcountcache->get_partition(current_iterate_pos).lock.get();
    MUTEX_TRY_LOCK_FOR(lock_bucket, bucket_mutex, t, this);
    if (!lock_bucket.is_locked()) {
      // we couldn't get the bucket lock, let's just reschedule and try later.
      Debug("hostdb", "iterateEvent event=%d eventp=%p: reschedule due to not getting bucket mutex", event, e);
      mutex->thread_holding->schedule_in(this, HOST_DB_RETRY_PERIOD);
      return EVENT_CONT;
    }

    TSHashTable<RefCountCacheHashing> *partMap = hostDB.refcountcache->get_partition(current_iterate_pos).get_map();
    for (RefCountCachePartition<HostDBInfo>::iterator_type i = partMap->begin(); i != partMap->end(); ++i) {
      HostDBInfo *r = (HostDBInfo *)i.m_value->item.get();
      if (r && !r->is_failed()) {
        action.continuation->handleEvent(EVENT_INTERVAL, static_cast<void *>(r));
      }
    }

    current_iterate_pos++;
    // And reschedule ourselves to pickup the next bucket after HOST_DB_RETRY_PERIOD.
    Debug("hostdb", "iterateEvent event=%d eventp=%p: completed current iteration %ld of %ld", event, e, current_iterate_pos,
          hostDB.refcountcache->partition_count());
    mutex->thread_holding->schedule_in(this, HOST_DB_ITERATE_PERIOD);
    return EVENT_CONT;
  } else {
    Debug("hostdb", "iterateEvent event=%d eventp=%p: completed FINAL iteration %ld", event, e, current_iterate_pos);
    // if there are no more buckets, then we're done.
    action.continuation->handleEvent(EVENT_DONE, NULL);
    hostdb_cont_free(this);
  }

  return EVENT_DONE;
}

//
// Build the put message and send it
//
void
HostDBContinuation::do_put_response(ClusterMachine *m, HostDBInfo *r, Continuation *c)
{
  // don't remote fill round-robin DNS entries
  // if configured not to cluster them
  if (!hostdb_cluster || (!c && r->round_robin && !hostdb_cluster_round_robin))
    return;

  HostDB_put_message msg;
  int len = make_put_message(r, c, (char *)&msg, sizeof(HostDB_put_message));

  clusterProcessor.invoke_remote(m->pop_ClusterHandler(), PUT_HOSTINFO_CLUSTER_FUNCTION, (char *)&msg, len);
}

//
// Probe state
//
int
HostDBContinuation::probeEvent(int /* event ATS_UNUSED */, Event *e)
{
  ink_assert(!link.prev && !link.next);
  EThread *t = e ? e->ethread : this_ethread();

  MUTEX_TRY_LOCK_FOR(lock, action.mutex, t, action.continuation);
  if (!lock.is_locked()) {
    mutex->thread_holding->schedule_in(this, HOST_DB_RETRY_PERIOD);
    return EVENT_CONT;
  }

  if (action.cancelled) {
    hostdb_cont_free(this);
    return EVENT_DONE;
  }

  if (!hostdb_enable || (!*md5.host_name && !md5.ip.isValid())) {
    if (action.continuation)
      action.continuation->handleEvent(EVENT_HOST_DB_LOOKUP, NULL);
    if (from)
      do_put_response(from, 0, from_cont);
    hostdb_cont_free(this);
    return EVENT_DONE;
  }

  if (!force_dns) {
    // Do the probe
    //
    Ptr<HostDBInfo> r = probe(mutex.get(), md5, false);

    if (r)
      HOSTDB_INCREMENT_DYN_STAT(hostdb_total_hits_stat);

    if (action.continuation && r)
      reply_to_cont(action.continuation, r.get());

    // Respond to any remote node
    //
    if (from)
      do_put_response(from, r.get(), from_cont);

    // If it suceeds or it was a remote probe, we are done
    //
    if (r || from) {
      hostdb_cont_free(this);
      return EVENT_DONE;
    }
    // If it failed, do a remote probe
    //
    if (do_get_response(e))
      return EVENT_CONT;
  }
  // If there are no remote nodes to probe, do a DNS lookup
  //
  do_dns();
  return EVENT_DONE;
}

int
HostDBContinuation::set_check_pending_dns()
{
  Queue<HostDBContinuation> &q = hostDB.pending_dns_for_hash(md5.hash);
  HostDBContinuation *c        = q.head;
  for (; c; c = (HostDBContinuation *)c->link.next) {
    if (md5.hash == c->md5.hash) {
      Debug("hostdb", "enqueuing additional request");
      q.enqueue(this);
      return false;
    }
  }
  q.enqueue(this);
  return true;
}

void
HostDBContinuation::remove_trigger_pending_dns()
{
  Queue<HostDBContinuation> &q = hostDB.pending_dns_for_hash(md5.hash);
  q.remove(this);
  HostDBContinuation *c = q.head;
  Queue<HostDBContinuation> qq;
  while (c) {
    HostDBContinuation *n = (HostDBContinuation *)c->link.next;
    if (md5.hash == c->md5.hash) {
      Debug("hostdb", "dequeuing additional request");
      q.remove(c);
      qq.enqueue(c);
    }
    c = n;
  }
  while ((c = qq.dequeue()))
    c->handleEvent(EVENT_IMMEDIATE, NULL);
}

//
// Query the DNS processor
//
void
HostDBContinuation::do_dns()
{
  ink_assert(!action.cancelled);
  if (is_byname()) {
    Debug("hostdb", "DNS %s", md5.host_name);
    IpAddr tip;
    if (0 == tip.load(md5.host_name)) {
      // check 127.0.0.1 format // What the heck does that mean? - AMC
      if (action.continuation) {
        HostDBInfo *r = lookup_done(tip, md5.host_name, false, HOST_DB_MAX_TTL, NULL);

        reply_to_cont(action.continuation, r);
      }
      hostdb_cont_free(this);
      return;
    }
    ts::ConstBuffer hname(md5.host_name, md5.host_len);
    Ptr<RefCountedHostsFileMap> current_host_file_map = hostDB.hosts_file_ptr;
    HostsFileMap::iterator find_result                = current_host_file_map->hosts_file_map.find(hname);
    if (find_result != current_host_file_map->hosts_file_map.end()) {
      if (action.continuation) {
        // Set the TTL based on how much time remains until the next sync
        HostDBInfo *r =
          lookup_done(IpAddr(find_result->second), md5.host_name, false, current_host_file_map->next_sync_time - ink_time(), NULL);
        reply_to_cont(action.continuation, r);
      }
      hostdb_cont_free(this);
      return;
    }
  }
  if (hostdb_lookup_timeout) {
    timeout = mutex->thread_holding->schedule_in(this, HRTIME_SECONDS(hostdb_lookup_timeout));
  } else {
    timeout = NULL;
  }
  if (set_check_pending_dns()) {
    DNSProcessor::Options opt;
    opt.timeout        = dns_lookup_timeout;
    opt.host_res_style = host_res_style_for(md5.db_mark);
    SET_HANDLER((HostDBContHandler)&HostDBContinuation::dnsEvent);
    if (is_byname()) {
      if (md5.dns_server)
        opt.handler  = md5.dns_server->x_dnsH;
      pending_action = dnsProcessor.gethostbyname(this, md5.host_name, opt);
    } else if (is_srv()) {
      Debug("dns_srv", "SRV lookup of %s", md5.host_name);
      pending_action = dnsProcessor.getSRVbyname(this, md5.host_name, opt);
    } else {
      ip_text_buffer ipb;
      Debug("hostdb", "DNS IP %s", md5.ip.toString(ipb, sizeof ipb));
      pending_action = dnsProcessor.gethostbyaddr(this, &md5.ip, opt);
    }
  } else {
    SET_HANDLER((HostDBContHandler)&HostDBContinuation::dnsPendingEvent);
  }
}

//
// Handle the response (put message)
//
int
HostDBContinuation::clusterResponseEvent(int /*  event ATS_UNUSED */, Event *e)
{
  if (from_cont) {
    HostDBContinuation *c;
    for (c = (HostDBContinuation *)hostDB.remoteHostDBQueue[key_partition()].head; c; c = (HostDBContinuation *)c->link.next)
      if (c == from_cont)
        break;

    // Check to see that we have not already timed out
    //
    if (c) {
      action    = c;
      from_cont = 0;
      MUTEX_TRY_LOCK(lock, c->mutex, e->ethread);
      MUTEX_TRY_LOCK(lock2, c->action.mutex, e->ethread);
      if (!lock.is_locked() || !lock2.is_locked()) {
        e->schedule_in(HOST_DB_RETRY_PERIOD);
        return EVENT_CONT;
      }
      bool failed = missing || (round_robin && !hostdb_cluster_round_robin);
      action.continuation->handleEvent(EVENT_HOST_DB_GET_RESPONSE, failed ? 0 : this);
    }
  } else {
    action = 0;
    // just a remote fill
    ink_assert(!missing);
    lookup_done(md5.ip, md5.host_name, false, ttl, NULL);
  }
  hostdb_cont_free(this);
  return EVENT_DONE;
}

//
// Wait for the response (put message)
//
int
HostDBContinuation::clusterEvent(int event, Event *e)
{
  // remove ourselves from the queue
  //
  hostDB.remoteHostDBQueue[key_partition()].remove(this);

  switch (event) {
  default:
    ink_assert(!"bad case");
    hostdb_cont_free(this);
    return EVENT_DONE;

  // handle the put response, e is really a HostDBContinuation *
  //
  case EVENT_HOST_DB_GET_RESPONSE:
    if (timeout) {
      timeout->cancel(this);
      timeout = NULL;
    }
    if (e) {
      HostDBContinuation *c         = (HostDBContinuation *)e;
      HostDBInfo *r                 = lookup_done(md5.ip, c->md5.host_name, false, c->ttl, NULL);
      r->app.allotment.application1 = c->app.allotment.application1;
      r->app.allotment.application2 = c->app.allotment.application2;

      HOSTDB_INCREMENT_DYN_STAT(hostdb_total_hits_stat);

      if (!action.cancelled) {
        if (reply_to_cont(action.continuation, r)) {
          // if we are not the owner and neither was the sender,
          // fill the owner
          //
          if (hostdb_migrate_on_demand) {
            ClusterMachine *m = cluster_machine_at_depth(master_hash(md5.hash));
            if (m && m != c->from)
              do_put_response(m, r, NULL);
          }
        }
      }
      hostdb_cont_free(this);
      return EVENT_DONE;
    }
    return failed_cluster_request(e);

  // did not get the put message in time
  //
  case EVENT_INTERVAL: {
    MUTEX_TRY_LOCK_FOR(lock, action.mutex, e->ethread, action.continuation);
    if (!lock.is_locked()) {
      e->schedule_in(HOST_DB_RETRY_PERIOD);
      return EVENT_CONT;
    }
    return failed_cluster_request(e);
  }
  }
}

int
HostDBContinuation::failed_cluster_request(Event *e)
{
  if (action.cancelled) {
    hostdb_cont_free(this);
    return EVENT_DONE;
  }
  // Attempt another remote probe
  //
  if (do_get_response(e))
    return EVENT_CONT;

  // Otherwise, do a DNS lookup
  //
  do_dns();
  return EVENT_DONE;
}

void
get_hostinfo_ClusterFunction(ClusterHandler *ch, void *data, int /* len ATS_UNUSED */)
{
  HostDBMD5 md5;
  HostDB_get_message *msg = (HostDB_get_message *)data;

  md5.host_name = msg->name;
  md5.host_len  = msg->namelen;
  md5.ip.assign(&msg->ip.sa);
  md5.port    = ats_ip_port_host_order(&msg->ip.sa);
  md5.hash    = msg->md5;
  md5.db_mark = db_mark_for(&msg->ip.sa);
#ifdef SPLIT_DNS
  SplitDNS *pSD  = 0;
  char *hostname = msg->name;
  if (hostname && SplitDNSConfig::isSplitDNSEnabled()) {
    pSD = SplitDNSConfig::acquire();

    if (0 != pSD) {
      md5.dns_server = static_cast<DNSServer *>(pSD->getDNSRecord(hostname));
    }
    SplitDNSConfig::release(pSD);
  }
#endif // SPLIT_DNS

  HostDBContinuation *c = hostDBContAllocator.alloc();
  HostDBContinuation::Options copt;
  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::probeEvent);
  c->from      = ch->machine;
  c->from_cont = msg->cont;

  /* -----------------------------------------
     we make a big assumption here! we presume
     that all the machines in the cluster are
     set to use the same configuration for
     DNS servers
     ----------------------------------------- */

  copt.host_res_style = host_res_style_for(&msg->ip.sa);
  c->init(md5, copt);
  c->mutex        = hostDB.refcountcache->lock_for_key(msg->md5.fold());
  c->action.mutex = c->mutex;
  dnsProcessor.thread->schedule_imm(c);
}

void
put_hostinfo_ClusterFunction(ClusterHandler *ch, void *data, int /* len ATS_UNUSED */)
{
  HostDB_put_message *msg = (HostDB_put_message *)data;
  HostDBContinuation *c   = hostDBContAllocator.alloc();
  HostDBContinuation::Options copt;
  HostDBMD5 md5;

  SET_CONTINUATION_HANDLER(c, (HostDBContHandler)&HostDBContinuation::clusterResponseEvent);
  md5.host_name = msg->name;
  md5.host_len  = msg->namelen;
  md5.ip.assign(&msg->ip.sa);
  md5.port            = ats_ip_port_host_order(&msg->ip.sa);
  md5.hash            = msg->md5;
  md5.db_mark         = db_mark_for(&msg->ip.sa);
  copt.host_res_style = host_res_style_for(&msg->ip.sa);
  c->init(md5, copt);
  c->mutex       = hostDB.refcountcache->lock_for_key(msg->md5.fold());
  c->from_cont   = msg->cont; // cannot use action if cont freed due to timeout
  c->missing     = msg->missing;
  c->round_robin = msg->round_robin;
  c->ttl         = msg->ttl;
  c->from        = ch->machine;
  dnsProcessor.thread->schedule_imm(c);
}

//
// Background event
// Just increment the current_interval.  Might do other stuff
// here, like move records to the current position in the cluster.
//
int
HostDBContinuation::backgroundEvent(int /* event ATS_UNUSED */, Event * /* e ATS_UNUSED */)
{
  // No nothing if hosts file checking is not enabled.
  if (hostdb_hostfile_check_interval == 0) {
    return EVENT_CONT;
  }

  hostdb_current_interval = ink_time();

  if ((hostdb_current_interval - hostdb_last_interval) > hostdb_hostfile_check_interval) {
    bool update_p = false; // do we need to reparse the file and update?
    struct stat info;
    char path[sizeof(hostdb_hostfile_path)];

    REC_ReadConfigString(path, "proxy.config.hostdb.host_file.path", sizeof(path));
    if (0 != strcasecmp(hostdb_hostfile_path, path)) {
      Debug("hostdb", "Update host file '%s' -> '%s'", (*hostdb_hostfile_path ? hostdb_hostfile_path : "*-none-*"),
            (*path ? path : "*-none-*"));
      // path to hostfile changed
      hostdb_hostfile_update_timestamp = 0; // never updated from this file
      if ('\0' != *path) {
        memcpy(hostdb_hostfile_path, path, sizeof(hostdb_hostfile_path));
      } else {
        hostdb_hostfile_path[0] = 0; // mark as not there
      }
      update_p = true;
    } else {
      hostdb_last_interval = hostdb_current_interval;
      if (*hostdb_hostfile_path) {
        if (0 == stat(hostdb_hostfile_path, &info)) {
          if (info.st_mtime > (time_t)hostdb_hostfile_update_timestamp) {
            update_p = true; // same file but it's changed.
          }
        } else {
          Debug("hostdb", "Failed to stat host file '%s'", hostdb_hostfile_path);
        }
      }
    }
    if (update_p) {
      Debug("hostdb", "Updating from host file");
      ParseHostFile(hostdb_hostfile_path, hostdb_hostfile_check_interval);
    }
  }

  return EVENT_CONT;
}

char *
HostDBInfo::hostname() const
{
  if (!reverse_dns)
    return NULL;

  return (char *)this + data.hostname_offset;
}

/*
 * The perm_hostname exists for all records not just reverse dns records.
 */
char *
HostDBInfo::perm_hostname() const
{
  if (hostname_offset == 0)
    return NULL;

  return (char *)this + hostname_offset;
}

HostDBRoundRobin *
HostDBInfo::rr()
{
  if (!round_robin)
    return NULL;

  return (HostDBRoundRobin *)((char *)this + this->app.rr.offset);
}

ClusterMachine *
HostDBContinuation::master_machine(ClusterConfiguration *cc)
{
  return cc->machine_hash((int)(md5.hash[1] >> 32));
}

struct ShowHostDB;
typedef int (ShowHostDB::*ShowHostDBEventHandler)(int event, Event *data);
struct ShowHostDB : public ShowCont {
  char *name;
  uint16_t port;
  IpEndpoint ip;
  bool force;
  bool output_json;
  int records_seen;

  int
  showMain(int event, Event *e)
  {
    CHECK_SHOW(begin("HostDB"));
    CHECK_SHOW(show("<a href=\"./showall\">Show all HostDB records<a/><hr>"));
    CHECK_SHOW(show("<form method = GET action = \"./name\">\n"
                    "Lookup by name (e.g. trafficserver.apache.org):<br>\n"
                    "<input type=text name=name size=64 maxlength=256>\n"
                    "</form>\n"
                    "<form method = GET action = \"./ip\">\n"
                    "Lookup by IP (e.g. 127.0.0.1):<br>\n"
                    "<input type=text name=ip size=64 maxlength=256>\n"
                    "</form>\n"
                    "<form method = GET action = \"./nameforce\">\n"
                    "Force DNS by name (e.g. trafficserver.apache.org):<br>\n"
                    "<input type=text name=name size=64 maxlength=256>\n"
                    "</form>\n"));
    return complete(event, e);
  }

  int
  showLookup(int /* event ATS_UNUSED */, Event * /* e ATS_UNUSED */)
  {
    SET_HANDLER(&ShowHostDB::showLookupDone);
    if (name) {
      HostDBProcessor::Options opts;
      opts.port  = port;
      opts.flags = HostDBProcessor::HOSTDB_DO_NOT_FORCE_DNS;
      hostDBProcessor.getbynameport_re(this, name, strlen(name), opts);
    } else
      hostDBProcessor.getbyaddr_re(this, &ip.sa);
    return EVENT_CONT;
  }

  int
  showAll(int event, Event *e)
  {
    if (!output_json) {
      CHECK_SHOW(begin("HostDB All Records"));
      CHECK_SHOW(show("<hr>"));
    } else {
      CHECK_SHOW(show("["));
    }
    SET_HANDLER(&ShowHostDB::showAllEvent);
    hostDBProcessor.iterate(this);
    return EVENT_CONT;
  }

  int
  showAllEvent(int event, Event *e)
  {
    if (event == EVENT_INTERVAL) {
      HostDBInfo *r = reinterpret_cast<HostDBInfo *>(e);
      if (output_json && records_seen++ > 0) {
        CHECK_SHOW(show(",")); // we need to seperate records
      }
      showOne(r, false, event, e);
      if (r->round_robin) {
        HostDBRoundRobin *rr_data = r->rr();
        if (rr_data) {
          if (!output_json) {
            CHECK_SHOW(show("<table border=1>\n"));
            CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Total", rr_data->rrcount));
            CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Good", rr_data->good));
            CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Current", rr_data->current));
            CHECK_SHOW(show("</table>\n"));
          } else {
            CHECK_SHOW(show(",\"%s\":\"%d\",", "rr_total", rr_data->rrcount));
            CHECK_SHOW(show("\"%s\":\"%d\",", "rr_good", rr_data->good));
            CHECK_SHOW(show("\"%s\":\"%d\",", "rr_current", rr_data->current));
            CHECK_SHOW(show("\"rr_records\":["));
          }

          for (int i = 0; i < rr_data->rrcount; i++) {
            showOne(&rr_data->info(i), true, event, e, rr_data);
            if (output_json) {
              CHECK_SHOW(show("}")); // we need to seperate records
              if (i < (rr_data->rrcount - 1))
                CHECK_SHOW(show(","));
            }
          }

          if (!output_json) {
            CHECK_SHOW(show("<br />\n<br />\n"));
          } else {
            CHECK_SHOW(show("]"));
          }
        }
      }

      if (output_json) {
        CHECK_SHOW(show("}"));
      }

    } else if (event == EVENT_DONE) {
      if (output_json) {
        CHECK_SHOW(show("]"));
        return completeJson(event, e);
      } else {
        return complete(event, e);
      }
    } else {
      ink_assert(!"unexpected event");
    }
    return EVENT_CONT;
  }

  int
  showOne(HostDBInfo *r, bool rr, int event, Event *e, HostDBRoundRobin *hostdb_rr = NULL)
  {
    ip_text_buffer b;
    if (!output_json) {
      CHECK_SHOW(show("<table border=1>\n"));
      CHECK_SHOW(show("<tr><td>%s</td><td>%s%s %s</td></tr>\n", "Type", r->round_robin ? "Round-Robin" : "",
                      r->reverse_dns ? "Reverse DNS" : "", r->is_srv ? "SRV" : "DNS"));

      if (r->perm_hostname()) {
        CHECK_SHOW(show("<tr><td>%s</td><td>%s</td></tr>\n", "Hostname", r->perm_hostname()));
      } else if (rr && r->is_srv && hostdb_rr) {
        CHECK_SHOW(show("<tr><td>%s</td><td>%s</td></tr>\n", "Hostname", r->srvname(hostdb_rr)));
      }

      // Let's display the MD5.
      CHECK_SHOW(show("<tr><td>%s</td><td>%u</td></tr>\n", "App1", r->app.allotment.application1));
      CHECK_SHOW(show("<tr><td>%s</td><td>%u</td></tr>\n", "App2", r->app.allotment.application2));
      CHECK_SHOW(show("<tr><td>%s</td><td>%u</td></tr>\n", "LastFailure", r->app.http_data.last_failure));
      if (!rr) {
        CHECK_SHOW(show("<tr><td>%s</td><td>%s</td></tr>\n", "Stale", r->is_ip_stale() ? "Yes" : "No"));
        CHECK_SHOW(show("<tr><td>%s</td><td>%s</td></tr>\n", "Timed-Out", r->is_ip_timeout() ? "Yes" : "No"));
        CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "TTL", r->ip_time_remaining()));
      }

      if (rr && r->is_srv) {
        CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Weight", r->data.srv.srv_weight));
        CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Priority", r->data.srv.srv_priority));
        CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Port", r->data.srv.srv_port));
        CHECK_SHOW(show("<tr><td>%s</td><td>%x</td></tr>\n", "Key", r->data.srv.key));
      } else if (!r->is_srv) {
        CHECK_SHOW(show("<tr><td>%s</td><td>%s</td></tr>\n", "IP", ats_ip_ntop(r->ip(), b, sizeof b)));
      }

      CHECK_SHOW(show("</table>\n"));
    } else {
      CHECK_SHOW(show("{"));
      CHECK_SHOW(show("\"%s\":\"%s%s%s\",", "type", (r->round_robin && !r->is_srv) ? "roundrobin" : "",
                      r->reverse_dns ? "reversedns" : "", r->is_srv ? "srv" : "dns"));

      if (r->perm_hostname()) {
        CHECK_SHOW(show("\"%s\":\"%s\",", "hostname", r->perm_hostname()));
      } else if (rr && r->is_srv && hostdb_rr) {
        CHECK_SHOW(show("\"%s\":\"%s\",", "hostname", r->srvname(hostdb_rr)));
      }

      CHECK_SHOW(show("\"%s\":\"%u\",", "app1", r->app.allotment.application1));
      CHECK_SHOW(show("\"%s\":\"%u\",", "app2", r->app.allotment.application2));
      CHECK_SHOW(show("\"%s\":\"%u\",", "lastfailure", r->app.http_data.last_failure));
      if (!rr) {
        CHECK_SHOW(show("\"%s\":\"%s\",", "stale", r->is_ip_stale() ? "yes" : "no"));
        CHECK_SHOW(show("\"%s\":\"%s\",", "timedout", r->is_ip_timeout() ? "yes" : "no"));
        CHECK_SHOW(show("\"%s\":\"%d\",", "ttl", r->ip_time_remaining()));
      }

      if (rr && r->is_srv) {
        CHECK_SHOW(show("\"%s\":\"%d\",", "weight", r->data.srv.srv_weight));
        CHECK_SHOW(show("\"%s\":\"%d\",", "priority", r->data.srv.srv_priority));
        CHECK_SHOW(show("\"%s\":\"%d\",", "port", r->data.srv.srv_port));
        CHECK_SHOW(show("\"%s\":\"%x\",", "key", r->data.srv.key));
      } else if (!r->is_srv) {
        CHECK_SHOW(show("\"%s\":\"%s\"", "ip", ats_ip_ntop(r->ip(), b, sizeof b)));
      }
    }
    return EVENT_CONT;
  }

  int
  showLookupDone(int event, Event *e)
  {
    HostDBInfo *r = (HostDBInfo *)e;

    CHECK_SHOW(begin("HostDB Lookup"));
    if (name) {
      CHECK_SHOW(show("<H2>%s</H2>\n", name));
    } else {
      CHECK_SHOW(show("<H2>%u.%u.%u.%u</H2>\n", PRINT_IP(ip)));
    }
    if (r) {
      showOne(r, false, event, e);
      if (r->round_robin) {
        HostDBRoundRobin *rr_data = r->rr();
        if (rr_data) {
          CHECK_SHOW(show("<table border=1>\n"));
          CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Total", rr_data->rrcount));
          CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Good", rr_data->good));
          CHECK_SHOW(show("<tr><td>%s</td><td>%d</td></tr>\n", "Current", rr_data->current));
          CHECK_SHOW(show("</table>\n"));

          for (int i = 0; i < rr_data->rrcount; i++)
            showOne(&rr_data->info(i), true, event, e, rr_data);
        }
      }
    } else {
      if (!name) {
        ip_text_buffer b;
        CHECK_SHOW(show("<H2>%s Not Found</H2>\n", ats_ip_ntop(&ip.sa, b, sizeof b)));
      } else {
        CHECK_SHOW(show("<H2>%s Not Found</H2>\n", name));
      }
    }
    return complete(event, e);
  }

  ShowHostDB(Continuation *c, HTTPHdr *h) : ShowCont(c, h), name(0), port(0), force(0), output_json(false), records_seen(0)
  {
    ats_ip_invalidate(&ip);
    SET_HANDLER(&ShowHostDB::showMain);
  }
};

#define STR_LEN_EQ_PREFIX(_x, _l, _s) (!ptr_len_ncasecmp(_x, _l, _s, sizeof(_s) - 1))

static Action *
register_ShowHostDB(Continuation *c, HTTPHdr *h)
{
  ShowHostDB *s = new ShowHostDB(c, h);
  int path_len;
  const char *path = h->url_get()->path_get(&path_len);

  SET_CONTINUATION_HANDLER(s, &ShowHostDB::showMain);
  if (STR_LEN_EQ_PREFIX(path, path_len, "ip")) {
    s->force = !ptr_len_ncasecmp(path + 3, path_len - 3, "force", 5);
    int query_len;
    const char *query = h->url_get()->query_get(&query_len);
    s->sarg           = ats_strndup(query, query_len);
    char *gn          = NULL;
    if (s->sarg)
      gn = (char *)memchr(s->sarg, '=', strlen(s->sarg));
    if (gn) {
      ats_ip_pton(gn + 1, &s->ip); // hope that's null terminated.
    }
    SET_CONTINUATION_HANDLER(s, &ShowHostDB::showLookup);
  } else if (STR_LEN_EQ_PREFIX(path, path_len, "name")) {
    s->force = !ptr_len_ncasecmp(path + 5, path_len - 5, "force", 5);
    int query_len;
    const char *query = h->url_get()->query_get(&query_len);
    s->sarg           = ats_strndup(query, query_len);
    char *gn          = NULL;
    if (s->sarg)
      gn = (char *)memchr(s->sarg, '=', strlen(s->sarg));
    if (gn) {
      s->name   = gn + 1;
      char *pos = strstr(s->name, "%3A");
      if (pos != NULL) {
        s->port = atoi(pos + 3);
        *pos    = '\0'; // Null terminate name
      } else {
        s->port = 0;
      }
    }
    SET_CONTINUATION_HANDLER(s, &ShowHostDB::showLookup);
  } else if (STR_LEN_EQ_PREFIX(path, path_len, "showall")) {
    int query_len     = 0;
    const char *query = h->url_get()->query_get(&query_len);
    if (query && query_len && strstr(query, "json")) {
      s->output_json = true;
    }
    Debug("hostdb", "dumping all hostdb records");
    SET_CONTINUATION_HANDLER(s, &ShowHostDB::showAll);
  }
  this_ethread()->schedule_imm(s);
  return &s->action;
}

#define HOSTDB_TEST_MAX_OUTSTANDING 100
#define HOSTDB_TEST_LENGTH 100000

struct HostDBTestReverse;
typedef int (HostDBTestReverse::*HostDBTestReverseHandler)(int, void *);
struct HostDBTestReverse : public Continuation {
  RegressionTest *test;
  int type;
  int *status;

  int outstanding;
  int total;
#if HAVE_LRAND48_R
  struct drand48_data dr;
#endif

  int
  mainEvent(int event, Event *e)
  {
    if (event == EVENT_HOST_DB_LOOKUP) {
      HostDBInfo *i = (HostDBInfo *)e;
      if (i) {
        rprintf(test, "HostDBTestReverse: reversed %s\n", i->hostname());
      }
      outstanding--;
    }
    while (outstanding < HOSTDB_TEST_MAX_OUTSTANDING && total < HOSTDB_TEST_LENGTH) {
      long l = 0;
#if HAVE_LRAND48_R
      lrand48_r(&dr, &l);
#else
      l = lrand48();
#endif
      IpEndpoint ip;
      ip.sin.sin_addr.s_addr = static_cast<in_addr_t>(l);
      outstanding++;
      total++;
      if (!(outstanding % 1000)) {
        rprintf(test, "HostDBTestReverse: %d\n", total);
      }
      hostDBProcessor.getbyaddr_re(this, &ip.sa);
    }
    if (!outstanding) {
      rprintf(test, "HostDBTestReverse: done\n");
      *status = REGRESSION_TEST_PASSED; //  TODO: actually verify it passed
      delete this;
    }
    return EVENT_CONT;
  }
  HostDBTestReverse(RegressionTest *t, int atype, int *astatus)
    : Continuation(new_ProxyMutex()), test(t), type(atype), status(astatus), outstanding(0), total(0)
  {
    SET_HANDLER((HostDBTestReverseHandler)&HostDBTestReverse::mainEvent);
#if HAVE_SRAND48_R
    srand48_r(time(NULL), &dr);
#else
    srand48(time(NULL));
#endif
  }
};

#if TS_HAS_TESTS
REGRESSION_TEST(HostDBTests)(RegressionTest *t, int atype, int *pstatus)
{
  eventProcessor.schedule_imm(new HostDBTestReverse(t, atype, pstatus), ET_CACHE);
}
#endif

RecRawStatBlock *hostdb_rsb;

void
ink_hostdb_init(ModuleVersion v)
{
  static int init_called = 0;

  ink_release_assert(!checkModuleVersion(v, HOSTDB_MODULE_VERSION));
  if (init_called)
    return;

  init_called = 1;
  // do one time stuff
  // create a stat block for HostDBStats
  hostdb_rsb = RecAllocateRawStatBlock((int)HostDB_Stat_Count);

  //
  // Register stats
  //

  RecRegisterRawStat(hostdb_rsb, RECT_PROCESS, "proxy.process.hostdb.total_lookups", RECD_INT, RECP_PERSISTENT,
                     (int)hostdb_total_lookups_stat, RecRawStatSyncSum);

  RecRegisterRawStat(hostdb_rsb, RECT_PROCESS, "proxy.process.hostdb.total_hits", RECD_INT, RECP_PERSISTENT,
                     (int)hostdb_total_hits_stat, RecRawStatSyncSum);

  RecRegisterRawStat(hostdb_rsb, RECT_PROCESS, "proxy.process.hostdb.ttl", RECD_FLOAT, RECP_PERSISTENT, (int)hostdb_ttl_stat,
                     RecRawStatSyncAvg);

  RecRegisterRawStat(hostdb_rsb, RECT_PROCESS, "proxy.process.hostdb.ttl_expires", RECD_INT, RECP_PERSISTENT,
                     (int)hostdb_ttl_expires_stat, RecRawStatSyncSum);

  RecRegisterRawStat(hostdb_rsb, RECT_PROCESS, "proxy.process.hostdb.re_dns_on_reload", RECD_INT, RECP_PERSISTENT,
                     (int)hostdb_re_dns_on_reload_stat, RecRawStatSyncSum);

  ts_host_res_global_init();
}

/// Pair of IP address and host name from a host file.
struct HostFilePair {
  typedef HostFilePair self;
  IpAddr ip;
  char const *name;
};

struct HostDBFileContinuation : public Continuation {
  typedef HostDBFileContinuation self;

  int idx;          ///< Working index.
  char const *name; ///< Host name (just for debugging)
  INK_MD5 md5;      ///< Key for entry.
  typedef std::vector<INK_MD5> Keys;
  Keys *keys;          ///< Entries from file.
  ats_scoped_str path; ///< Used to keep the host file name around.

  HostDBFileContinuation() : Continuation(0) {}
  /// Finish update
  static void finish(Keys *keys ///< Valid keys from update.
                     );
  /// Clean up this instance.
  void destroy();
};

ClassAllocator<HostDBFileContinuation> hostDBFileContAllocator("hostDBFileContAllocator");

void
HostDBFileContinuation::destroy()
{
  this->~HostDBFileContinuation();
  hostDBFileContAllocator.free(this);
}

// Host file processing globals.

// We can't allow more than one update to be
// proceeding at a time in any case so we might as well make these
// globals.
int HostDBFileUpdateActive = 0;

static void
ParseHostLine(Ptr<RefCountedHostsFileMap> &map, char *l)
{
  Tokenizer elts(" \t");
  int n_elts = elts.Initialize(l, SHARE_TOKS);

  // Elements should be the address then a list of host names.
  // Don't use RecHttpLoadIp because the address *must* be literal.
  IpAddr ip;
  if (n_elts > 1 && 0 == ip.load(elts[0])) {
    for (int i = 1; i < n_elts; ++i) {
      ts::ConstBuffer name(elts[i], strlen(elts[i]));
      // If we don't have an entry already (host files only support single IPs for a given name)
      if (map->hosts_file_map.find(name) == map->hosts_file_map.end()) {
        map->hosts_file_map[name] = ip;
      }
    }
  }
}

void
ParseHostFile(char const *path, unsigned int hostdb_hostfile_check_interval)
{
  Ptr<RefCountedHostsFileMap> parsed_hosts_file_ptr;

  // Test and set for update in progress.
  if (0 != ink_atomic_swap(&HostDBFileUpdateActive, 1)) {
    Debug("hostdb", "Skipped load of host file because update already in progress");
    return;
  }
  Debug("hostdb", "Loading host file '%s'", path);

  if (*path) {
    ats_scoped_fd fd(open(path, O_RDONLY));
    if (fd >= 0) {
      struct stat info;
      if (0 == fstat(fd, &info)) {
        // +1 in case no terminating newline
        int64_t size = info.st_size + 1;

        parsed_hosts_file_ptr                 = new RefCountedHostsFileMap;
        parsed_hosts_file_ptr->next_sync_time = ink_time() + hostdb_hostfile_check_interval;
        parsed_hosts_file_ptr->HostFileText   = static_cast<char *>(ats_malloc(size));
        if (parsed_hosts_file_ptr->HostFileText) {
          char *base = parsed_hosts_file_ptr->HostFileText;
          char *limit;

          size   = read(fd, parsed_hosts_file_ptr->HostFileText, info.st_size);
          limit  = parsed_hosts_file_ptr->HostFileText + size;
          *limit = 0;

          // We need to get a list of all name/addr pairs so that we can
          // group names for round robin records. Also note that the
          // pairs have pointer back in to the text storage for the file
          // so we need to keep that until we're done with @a pairs.
          while (base < limit) {
            char *spot = strchr(base, '\n');

            // terminate the line.
            if (0 == spot)
              spot = limit; // no trailing EOL, grab remaining
            else
              *spot = 0;

            while (base < spot && isspace(*base))
              ++base;                        // skip leading ws
            if (*base != '#' && base < spot) // non-empty non-comment line
              ParseHostLine(parsed_hosts_file_ptr, base);
            base = spot + 1;
          }

          hostdb_hostfile_update_timestamp = hostdb_current_interval;
        }
      }
    }
  }

  // Swap the pointer
  hostDB.hosts_file_ptr = parsed_hosts_file_ptr;
  // Mark this one as completed, so we can allow another update to happen
  HostDBFileUpdateActive = 0;
}

//
// Regression tests
//
// Take a started hostDB and fill it up and make sure it doesn't explode
#ifdef TS_HAS_TESTS
struct HostDBRegressionContinuation;

struct HostDBRegressionContinuation : public Continuation {
  int hosts;
  const char **hostnames;
  RegressionTest *test;
  int type;
  int *status;

  int success;
  int failure;
  int outstanding;
  int i;

  int
  mainEvent(int event, HostDBInfo *r)
  {
    (void)event;

    if (event == EVENT_INTERVAL) {
      rprintf(test, "hosts=%d success=%d failure=%d outstanding=%d i=%d\n", hosts, success, failure, outstanding, i);
    }
    if (event == EVENT_HOST_DB_LOOKUP) {
      --outstanding;
      // since this is a lookup done, data is either hostdbInfo or NULL
      if (r) {
        rprintf(test, "hostdbinfo r=%x\n", r);
        rprintf(test, "hostdbinfo hostname=%s\n", r->perm_hostname());
        rprintf(test, "hostdbinfo rr %x\n", r->rr());
        // If RR, print all of the enclosed records
        if (r->rr()) {
          rprintf(test, "hostdbinfo good=%d\n", r->rr()->good);
          for (int x = 0; x < r->rr()->good; x++) {
            ip_port_text_buffer ip_buf;
            ats_ip_ntop(r->rr()->info(x).ip(), ip_buf, sizeof(ip_buf));
            rprintf(test, "hostdbinfo RR%d ip=%s\n", x, ip_buf);
          }
        } else { // Otherwise, just the one will do
          ip_port_text_buffer ip_buf;
          ats_ip_ntop(r->ip(), ip_buf, sizeof(ip_buf));
          rprintf(test, "hostdbinfo A ip=%s\n", ip_buf);
        }
        ++success;
      } else {
        ++failure;
      }
    }

    if (i < hosts) {
      hostDBProcessor.getbyname_re(this, hostnames[i++], 0);
      return EVENT_CONT;
    } else {
      rprintf(test, "HostDBTestRR: %d outstanding %d succcess %d failure\n", outstanding, success, failure);
      if (success == hosts) {
        *status = REGRESSION_TEST_PASSED;
      } else {
        *status = REGRESSION_TEST_FAILED;
      }
      return EVENT_DONE;
    }
  }

  HostDBRegressionContinuation(int ahosts, const char **ahostnames, RegressionTest *t, int atype, int *astatus)
    : Continuation(new_ProxyMutex()),
      hosts(ahosts),
      hostnames(ahostnames),
      test(t),
      type(atype),
      status(astatus),
      success(0),
      failure(0),
      i(0)
  {
    outstanding = ahosts;
    SET_HANDLER(&HostDBRegressionContinuation::mainEvent);
  }
};

static const char *dns_test_hosts[] = {
  "www.apple.com", "www.ibm.com", "www.microsoft.com",
  "www.coke.com", // RR record
  "4.2.2.2",      // An IP-- since we don't expect resolution
  "127.0.0.1",    // loopback since it has some special handling
};

REGRESSION_TEST(HostDBProcessor)(RegressionTest *t, int atype, int *pstatus)
{
  eventProcessor.schedule_in(new HostDBRegressionContinuation(6, dns_test_hosts, t, atype, pstatus), HRTIME_SECONDS(1));
}

#endif
