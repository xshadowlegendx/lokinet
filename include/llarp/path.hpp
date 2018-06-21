#ifndef LLARP_PATH_HPP
#define LLARP_PATH_HPP
#include <llarp/path.h>
#include <llarp/router.h>
#include <llarp/time.h>
#include <llarp/aligned.hpp>
#include <llarp/crypto.hpp>
#include <llarp/endpoint.hpp>
#include <llarp/messages/relay_ack.hpp>
#include <llarp/messages/relay_commit.hpp>
#include <llarp/path_types.hpp>
#include <llarp/router_id.hpp>

#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace llarp
{
  struct TransitHopInfo
  {
    TransitHopInfo() = default;
    TransitHopInfo(const RouterID& down, const LR_CommitRecord& record);

    PathID_t pathID;
    RouterID upstream;
    RouterID downstream;

    friend std::ostream&
    operator<<(std::ostream& out, const TransitHopInfo& info)
    {
      out << "<Transit Hop id=" << info.pathID;
      out << " upstream=" << info.upstream << " downstream=" << info.downstream;
      return out << ">";
    }

    bool
    operator==(const TransitHopInfo& other) const
    {
      return pathID == other.pathID && upstream == other.upstream
          && downstream == other.downstream;
    }

    bool
    operator!=(const TransitHopInfo& other) const
    {
      return !(*this == other);
    }

    bool
    operator<(const TransitHopInfo& other) const
    {
      return pathID < other.pathID || upstream < other.upstream
          || downstream < other.downstream;
    }

    struct Hash
    {
      std::size_t
      operator()(TransitHopInfo const& a) const
      {
        std::size_t idx0, idx1, idx2;
        memcpy(&idx0, a.upstream, sizeof(std::size_t));
        memcpy(&idx1, a.downstream, sizeof(std::size_t));
        memcpy(&idx2, a.pathID, sizeof(std::size_t));
        return idx0 ^ idx1 ^ idx2;
      }
    };
  };

  struct PathIDHash
  {
    std::size_t
    operator()(const PathID_t& a) const
    {
      std::size_t idx0;
      memcpy(&idx0, a, sizeof(std::size_t));
      return idx0;
    }
  };

  struct TransitHop
  {
    TransitHop() = default;

    TransitHopInfo info;
    SharedSecret pathKey;
    llarp_time_t started;
    // 10 minutes default
    llarp_time_t lifetime = 360000;
    llarp_proto_version_t version;

    bool
    Expired(llarp_time_t now) const;

    // forward data in upstream direction
    void
    ForwardUpstream(llarp_buffer_t X, const TunnelNonce& Y, llarp_router* r);

    // forward data in downstream direction
    void
    ForwardDownstream(llarp_buffer_t X, const TunnelNonce& Y, llarp_router* r);
  };

  /// configuration for a single hop when building a path
  struct PathHopConfig
  {
    /// path id
    PathID_t pathID;
    // router contact of router
    llarp_rc router;
    // temp public encryption key
    SecretKey commkey;
    /// shared secret at this hop
    SharedSecret shared;
    /// next hop's router id
    RouterID upstream;
    /// nonce for key exchange
    TunnelNonce nonce;

    ~PathHopConfig();
    PathHopConfig();
  };

  enum PathStatus
  {
    ePathBuilding,
    ePathEstablished,
    ePathTimeout,
    ePathExpired
  };

  /// A path we made
  struct Path
  {
    typedef std::vector< PathHopConfig > HopList;
    HopList hops;
    llarp_time_t buildStarted;
    PathStatus status;

    Path(llarp_path_hops* path);

    void
    EncryptAndSend(llarp_buffer_t buf, llarp_router* r);

    void
    DecryptAndRecv(llarp_buffer_t buf, IEndpointHandler* handler);

    const PathID_t&
    PathID() const;

    RouterID
    Upstream();
  };

  template < typename User >
  struct AsyncPathKeyExchangeContext
  {
    Path* path = nullptr;
    typedef void (*Handler)(AsyncPathKeyExchangeContext< User >*);
    User* user               = nullptr;
    Handler result           = nullptr;
    size_t idx               = 0;
    llarp_threadpool* worker = nullptr;
    llarp_logic* logic       = nullptr;
    llarp_crypto* crypto     = nullptr;
    LR_CommitMessage* LRCM   = nullptr;

    static void
    HandleDone(void* u)
    {
      AsyncPathKeyExchangeContext< User >* ctx =
          static_cast< AsyncPathKeyExchangeContext< User >* >(u);
      ctx->result(ctx);
    }

    static void
    GenerateNextKey(void* u)
    {
      AsyncPathKeyExchangeContext< User >* ctx =
          static_cast< AsyncPathKeyExchangeContext< User >* >(u);

      auto& hop = ctx->path->hops[ctx->idx];
      // generate key
      ctx->crypto->encryption_keygen(hop.commkey);
      hop.nonce.Randomize();
      // do key exchange
      if(!ctx->crypto->dh_client(hop.shared, hop.router.enckey, hop.commkey,
                                 hop.nonce))
      {
        llarp::Error("Failed to generate shared key for path build");
        abort();
        return;
      }
      // randomize hop's path id
      hop.pathID.Randomize();

      LR_CommitRecord record;

      auto& frame = ctx->LRCM->frames[ctx->idx];
      ++ctx->idx;
      if(ctx->idx < ctx->path->hops.size())
      {
        hop.upstream = ctx->path->hops[ctx->idx].router.pubkey;
      }
      else
      {
        hop.upstream = hop.router.pubkey;
      }
      auto buf = frame.Buffer();
      buf->cur = buf->base + EncryptedFrame::OverheadSize;
      // generate record
      if(!record.BEncode(buf))
      {
        // failed to encode?
        llarp::Error("Failed to generate Commit Record");
        abort();
        return;
      }
      // rewind
      buf->cur = buf->base;

      if(!frame.EncryptInPlace(hop.commkey, hop.router.enckey, ctx->crypto))
      {
        llarp::Error("Failed to encrypt LRCR");
        abort();
        return;
      }

      if(ctx->idx < ctx->path->hops.size())
      {
        // next hop
        llarp_threadpool_queue_job(ctx->worker, {ctx, &GenerateNextKey});
      }
      else
      {
        // farthest hop
        llarp_logic_queue_job(ctx->logic, {ctx, &HandleDone});
      }
    }

    AsyncPathKeyExchangeContext(llarp_crypto* c) : crypto(c)
    {
    }

    /// Generate all keys asynchronously and call hadler when done
    void
    AsyncGenerateKeys(Path* p, llarp_logic* l, llarp_threadpool* pool, User* u,
                      Handler func)
    {
      path   = p;
      logic  = l;
      user   = u;
      result = func;
      worker = pool;
      LRCM   = new LR_CommitMessage;

      for(size_t idx = 0; idx < MAXHOPS; ++idx)
      {
        LRCM->frames.emplace_back(256);
        LRCM->frames.back().Randomize();
      }
      llarp_threadpool_queue_job(pool, {this, &GenerateNextKey});
    }
  };

  enum PathBuildStatus
  {
    ePathBuildSuccess,
    ePathBuildTimeout,
    ePathBuildReject
  };

  struct PathContext
  {
    PathContext(llarp_router* router);
    ~PathContext();

    /// called from router tick function
    void
    ExpirePaths();

    void
    AllowTransit();
    void
    RejectTransit();

    bool
    AllowingTransit() const;

    bool
    HasTransitHop(const TransitHopInfo& info);

    bool
    HandleRelayCommit(const LR_CommitMessage* msg);

    bool
    HandleRelayAck(const LR_AckMessage* msg);

    void
    PutTransitHop(const TransitHop& hop);

    bool
    ForwardLRCM(const RouterID& nextHop, std::deque< EncryptedFrame >& frames);

    bool
    HopIsUs(const PubKey& k) const;

    void
    AddOwnPath(Path* p);

    typedef std::unordered_multimap< PathID_t, TransitHop, PathIDHash >
        TransitHopsMap_t;

    typedef std::pair< std::mutex, TransitHopsMap_t > SyncTransitMap_t;

    typedef std::map< PathID_t, Path* > OwnedPathsMap_t;

    typedef std::pair< std::mutex, OwnedPathsMap_t > SyncOwnedPathsMap_t;

    llarp_threadpool*
    Worker();

    llarp_crypto*
    Crypto();

    llarp_logic*
    Logic();

    byte_t*
    EncryptionSecretKey();

    const byte_t*
    OurRouterID() const;

   private:
    llarp_router* m_Router;
    SyncTransitMap_t m_TransitPaths;
    SyncOwnedPathsMap_t m_OurPaths;

    bool m_AllowTransit;
  };
}  // namespace llarp

#endif
