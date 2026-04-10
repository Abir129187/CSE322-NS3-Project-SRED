#ifndef SRED_QUEUE_DISC_MOD_H
#define SRED_QUEUE_DISC_MOD_H

#include "queue-disc.h"

#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"

#include <vector>

namespace ns3
{

class TraceContainer;

//
// Extension of the SRED algorithm that integrates average queue delay
// into the drop probability decision logic. This addresses a limitation
// of original SRED which calculates drop probability based solely on
// queue length, ignoring the latency experienced by packets — a factor
// critical for real-time applications.
//
// The delay-aware modification introduces:
//
//   D(t)  — EWMA of measured sojourn time (queue delay), updated on
//           every dequeue event.
//
//   Dn(t) = min(1, D(t) / Dref)
//           Normalized delay, clamped to [0,1].  Dref is a configurable
//           reference delay that represents the "acceptable" delay target.
//
//   Pda(t) = Psred(q) * min(1, 1/(256 * P(t)^2)) * (1 + gamma * Dn(t)^k)
//           Delay-aware zap probability.  The term (1 + gamma * Dn(t)^k)
//           acts as a multiplicative penalty that grows super-linearly
//           (for k > 1) as the measured delay approaches or exceeds Dref.
//           gamma is a configurable weight factor controlling the strength
//           of the delay penalty.
//
// The Full SRED hit-multiplier (eq. 5.1) is still applied on top of Pda.
class SredQueueDiscMod : public QueueDisc
{
  public:
    // Get the type ID.
    //  the object TypeId
    static TypeId GetTypeId();

    // SredQueueDiscMod Constructor
    SredQueueDiscMod();

    //  Destructor
    ~SredQueueDiscMod() override;

    //  Drop types
    enum
    {
        DTYPE_NONE,     // Ok, no drop
        DTYPE_FORCED,   // A "forced" drop
        DTYPE_UNFORCED, // An "unforced" (random) drop
    };

    // Assign a fixed random variable stream number to the random variables
    // used by this model.
    //
    //  first stream index to use
    //  the number of stream indices assigned by this model
    int64_t AssignStreams(int64_t stream);

    //  Get the estimated number of active flows.
    //  estimated number of active flows, or 0 if P(t) == 0
    double GetFlowCountEstimate() const;

    //  Get the current average queue delay (EWMA).
    //  the EWMA of queue sojourn time D(t)
    Time GetAverageDelay() const;

    // Reasons for dropping packets
    static constexpr const char* UNFORCED_DROP = "Unforced drop"; // Early probability drops
    static constexpr const char* FORCED_DROP = "Forced drop";     // Forced drops, queue full
    // Reasons for marking packets
    static constexpr const char* UNFORCED_MARK = "Unforced mark"; // Early probability marks
    static constexpr const char* FORCED_MARK = "Forced mark";     // Forced marks

  protected:
    //  Dispose of the object
    void DoDispose() override;

  private:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;

    //  Initialize the queue parameters.
    void InitializeParams() override;

    //  A single entry in the zombie list.
    struct ZombieEntry
    {
        uint32_t flowHash; // Hash identifying the flow
        uint32_t count;    // Number of hits for this zombie
        Time timestamp;    // Time of last hit
        bool valid;        // Whether this entry has been populated

        ZombieEntry()
            : flowHash(0),
              count(0),
              timestamp(Seconds(0)),
              valid(false)
        {
        }
    };

    //  Perform the zombie list comparison for an arriving packet.
    //
    //  flowHash hash of the arriving packet's flow
    //  true if a hit occurred
    bool ZombieListCheck(uint32_t flowHash);

    //  Compute the DELAY-AWARE SRED drop probability.
    //
    // Implements:
    //   Step 1: Three-level step function p_sred(q)  (eq. 4.3)
    //   Step 2: Delay-aware zap probability           (MODIFIED)
    //           Pda(t) = p_sred(q) * min(1, 1/(256*P(t)^2))
    //                              * (1 + gamma * Dn(t)^k)
    //   Step 3: Full SRED hit-multiplier              (eq. 5.1)
    //   Step 4: Update EWMA P(t)
    //
    //  nQueued current number of packets (or bytes) in the queue
    //  hit whether the arriving packet caused a zombie-list hit
    //  the drop probability for this packet
    double CalculateDropProbability(uint32_t nQueued, bool hit);

    //  Original SRED parameters (user-configurable) 
    uint32_t m_meanPktSize;       // Average packet size
    double m_maxTh;               // Buffer size B — forced-drop threshold
    double m_pMax;                // Maximum early-drop probability in p_sred(q)
    uint32_t m_zombieListSize;    // Number of entries in the zombie list (M)
    double m_zombieOverwriteProb; // Probability of overwriting a zombie on a miss
    double m_hitProbEwma;         // EWMA weight alpha for hit-frequency estimator
    DataRate m_linkBandwidth;     // Link bandwidth
    Time m_linkDelay;             // Link delay
    bool m_useEcn;                // True if ECN is used
    bool m_useHardDrop;           // True if packets are always dropped when queue full
    bool m_fullSred;              // True to enable Full SRED (hit-multiplier boost)

    //  NEW: Delay-aware parameters (user-configurable) 
    double m_gamma;           // Weight factor for delay penalty term
    double m_delayK;          // Exponent k — penalizes large delays aggressively when k > 1
    Time m_dRef;              // Reference delay Dref for normalization
    double m_delayEwmaWeight; // EWMA weight beta for delay estimator D(t)

    //  Original SRED internal state 
    std::vector<ZombieEntry> m_zombieList; // The zombie list (flow cache)
    uint32_t m_zombiePopulated;            // Number of valid entries in zombie list
    double m_pHit;                         // EWMA estimate of hit probability P(t)
    double m_sredDropProb;                 // Current SRED drop probability

    //  NEW: Delay-aware internal state 
    double m_avgDelay; // EWMA of queue sojourn time D(t), in seconds

    Ptr<UniformRandomVariable> m_uv; // Rng stream
};

}; // namespace ns3

#endif // SRED_QUEUE_DISC_MOD_H
