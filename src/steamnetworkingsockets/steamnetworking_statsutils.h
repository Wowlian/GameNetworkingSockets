//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Utils for calculating networking stats
//
//=============================================================================

#ifndef STEAMNETWORKING_STATSUTILS_H
#define STEAMNETWORKING_STATSUTILS_H
#pragma once

#include <tier0/basetypes.h>
#include <tier0/t0constants.h>
#include "percentile_generator.h"
#include "steamnetworking_stats.h"
#include "steamnetworkingsockets_internal.h"

//#include <google/protobuf/repeated_field.h> // FIXME - should only need this!
#include <tier0/memdbgoff.h>
#include <steamnetworkingsockets_messages.pb.h>
#include <tier0/memdbgon.h>

class CMsgSteamDatagramConnectionQuality;

// Internal stuff goes in a private namespace
namespace SteamNetworkingSocketsLib {

/// Default interval for link stats rate measurement
const SteamNetworkingMicroseconds k_usecSteamDatagramLinkStatsDefaultInterval = 5 * k_nMillion;

/// Default interval for speed stats rate measurement
const SteamNetworkingMicroseconds k_usecSteamDatagramSpeedStatsDefaultInterval = 1 * k_nMillion;

/// We should send tracer ping requests in our packets on approximately
/// this interval.  (Tracer pings and their replies are relatively cheap.)
/// These serve both as latency measurements, and also as keepalives, if only
/// one side or the other is doing most of the talking, to make sure the other side
/// always does a minimum amount of acking.
const SteamNetworkingMicroseconds k_usecLinkStatsPingRequestInterval = 5 * k_nMillion;

/// Client should send instantaneous connection quality stats
/// at approximately this interval
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportMinInterval = 17 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportInterval = 20 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportMaxInterval = 30 * k_nMillion;

/// Client will report lifetime connection stats at approximately this interval
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportMinInterval = 102 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportInterval = 120 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportMaxInterval = 140 * k_nMillion;

/// If we are timing out, ping the peer on this interval
const SteamNetworkingMicroseconds k_usecAggressivePingInterval = 200*1000;

/// If we haven't heard from the peer in a while, send a keepalive
const SteamNetworkingMicroseconds k_usecKeepAliveInterval = 10*k_nMillion;

/// Track the rate that something is happening
struct Rate_t
{
	void Reset() { memset( this, 0, sizeof(*this) ); }

	int64	m_nTotal;
	int64	m_nCurrentInterval;
	//int64	m_nCurrentLongInterval;
	float	m_flRate;
	float	m_flPeakRate;

	inline void Process( int64 nIncrement )
	{
		m_nTotal += nIncrement;
		m_nCurrentInterval += nIncrement;
		//m_nCurrentLongInterval += nIncrement;
	}

	inline void UpdateInterval( float flIntervalDuration )
	{
		m_flRate = float(m_nCurrentInterval) / flIntervalDuration;
		m_flPeakRate = Max( m_flPeakRate, m_flRate );
		m_nCurrentInterval = 0;
	}

	inline void operator+=( const Rate_t &x )
	{
		m_nTotal += x.m_nTotal;
		m_nCurrentInterval += x.m_nCurrentInterval;
		//m_nCurrentLongInterval += x.m_nCurrentLongInterval;
		m_flRate += x.m_flRate;
		// !NOTE: Don't aggregate peak.  It's ambiguous whether we should take the sum or max.
	}
};

/// Track flow rate (number and bytes)
struct PacketRate_t
{
	void Reset() { memset( this, 0, sizeof(*this) ); }

	Rate_t m_packets;
	Rate_t m_bytes;

	inline void ProcessPacket( int sz )
	{
		m_packets.Process( 1 );
		m_bytes.Process( sz );
	}

	void UpdateInterval( float flIntervalDuration )
	{
		m_packets.UpdateInterval( flIntervalDuration );
		m_bytes.UpdateInterval( flIntervalDuration );
	}

	inline void operator+=( const PacketRate_t &x )
	{
		m_packets += x.m_packets;
		m_bytes += x.m_bytes;
	}
};

/// Class used to track ping values
struct PingTracker
{
	void Reset();

	/// Called when we receive a ping measurement
	void ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow );

	struct Ping
	{
		int m_nPingMS;
		SteamNetworkingMicroseconds m_usecTimeRecv;
	};

	/// Recent ping measurements.  The most recent one is at entry 0.
	Ping m_arPing[ 3 ];

	/// Number of valid entries in m_arPing.
	int m_nValidPings;

	/// Do we have a full sample?
	bool HasFullSample() const { return m_nValidPings >= V_ARRAYSIZE(m_arPing); }

	/// Time when the most recent ping was received
	SteamNetworkingMicroseconds TimeRecvMostRecentPing() const { return m_arPing[0].m_usecTimeRecv; }

	/// Time when the oldest ping was received
	SteamNetworkingMicroseconds TimeRecvOldestPing() const { return ( m_nValidPings > 0 ) ? m_arPing[m_nValidPings-1].m_usecTimeRecv : 0; }

	/// Ping estimate, being pessimistic
	int PessimisticPingEstimate() const;

	/// Ping estimate, being optimistic
	int OptimisticPingEstimate() const;

	/// Smoothed ping value
	int m_nSmoothedPing;

	/// Time when we last sent a message, for which we expect a reply (possibly delayed)
	/// that we could use to measure latency.  (Possibly because the reply contains
	/// a simple timestamp, or possibly because it will contain a sequence number, and
	/// we will be able to look up that sequence number and remember when we sent it.)
	SteamNetworkingMicroseconds m_usecTimeLastSentPingRequest;

	/// Total number of pings we have received
	inline int TotalPingsReceived() const { return m_sample.NumSamplesTotal(); }

	/// Should match CMsgSteamDatagramLinkLifetimeStats
	int m_nHistogram25;
	int m_nHistogram50;
	int m_nHistogram75;
	int m_nHistogram100;
	int m_nHistogram125;
	int m_nHistogram150;
	int m_nHistogram200;
	int m_nHistogram300;
	int m_nHistogramMax;

	/// Track sample of pings received so we can generate a histogram.
	/// Also tracks how many pings we have received total
	PercentileGenerator<uint16> m_sample;
};

/// Token bucket rate limiter
/// https://en.wikipedia.org/wiki/Token_bucket
struct TokenBucketRateLimiter
{
	TokenBucketRateLimiter() { Reset(); }

	/// Mark the token bucket as full and reset internal timer
	void Reset() { m_usecLastTime = 0; m_flTokenDeficitFromFull = 0.0f; }

	/// Attempt to spend a token.
	/// flMaxSteadyStateRate - the rate that tokens are added to the bucket, per second.
	///                        Over a long interval, tokens cannot be spent faster than this rate.  And if they are consumed
	///                        at this rate, there is no allowance for bursting higher.  Typically you'll set this to a bit
	///                        higher than the true steady-state rate, so that the bucket can fill back up to allow for
	///                        another burst.
	/// flMaxBurst - The max possible burst, in tokens.
	bool BCheck( SteamNetworkingMicroseconds usecNow, float flMaxSteadyStateRate, float flMaxBurst )
	{
		Assert( flMaxBurst >= 1.0f );
		Assert( flMaxSteadyStateRate > 0.0f );

		// Calculate elapsed time (in seconds) and advance timestamp
		float flElapsed = ( usecNow - m_usecLastTime ) * 1e-6f;
		m_usecLastTime = usecNow;

		// Add tokens to the bucket, but stop if it gets full
		m_flTokenDeficitFromFull = Max( m_flTokenDeficitFromFull - flElapsed*flMaxSteadyStateRate, 0.0f );

		// Burst rate currently being exceeded?
		if ( m_flTokenDeficitFromFull + 1.0f > flMaxBurst )
			return false;

		// We have a token.  Spend it
		m_flTokenDeficitFromFull += 1.0f;
		return true;
	}

private:

	/// Last time a token was spent
	SteamNetworkingMicroseconds m_usecLastTime;

	/// The degree to which the bucket is not full.  E.g. 0 is "full" and any higher number means they are less than full.
	/// Doing the accounting in this "inverted" way makes it easier to reset and adjust the limits dynamically.
	float m_flTokenDeficitFromFull;
};

/// Class used to handle link quality calculations.
struct LinkStatsTrackerBase
{

	/// Estimate a conservative (i.e. err on the large side) timeout for the connection
	SteamNetworkingMicroseconds CalcConservativeTimeout() const
	{
		return ( m_ping.m_nSmoothedPing >= 0 ) ? ( m_ping.m_nSmoothedPing*2 + 500000 ) : k_nMillion;
	}

	/// What version is the peer running?  It's 0 if we don't know yet.
	uint32 m_nPeerProtocolVersion;

	/// Ping
	PingTracker m_ping;

	//
	// Outgoing stats
	//
	int64 m_nNextSendSequenceNumber;
	PacketRate_t m_sent;
	SteamNetworkingMicroseconds m_usecTimeLastSentSeq;

	/// Called when we sent a packet, with or without a sequence number
	inline void TrackSentPacket( int cbPktSize )
	{
		m_sent.ProcessPacket( cbPktSize );
		++m_nPktsSentSinceSentInstantaneous;
		++m_nPktsSentSinceSentLifetime;
	}

	/// Consume the next sequence number, and record the time at which
	/// we sent a sequenced packet.  (Don't call this unless you are sending
	/// a sequenced packet.)
	inline uint16 GetNextSendSequenceNumber( SteamNetworkingMicroseconds usecNow )
	{
		m_usecTimeLastSentSeq = usecNow;
		return uint16( m_nNextSendSequenceNumber++ );
	}

	//
	// Incoming
	//
	int64 m_nLastRecvSequenceNumber;
	PacketRate_t m_recv;

	/// Packets that we receive that exceed the rate limit.
	/// (We might drop these, or we might just want to be interested in how often it happens.)
	PacketRate_t m_recvExceedRateLimit;

	/// Time when we last received anything
	SteamNetworkingMicroseconds m_usecTimeLastRecv;

	/// Time when we last received a sequenced packet
	SteamNetworkingMicroseconds m_usecTimeLastRecvSeq;

	/// Called when we receive any packet, with or without a sequence number.
	/// Does not perform any rate limiting checks
	inline void TrackRecvPacket( int cbPktSize, SteamNetworkingMicroseconds usecNow )
	{
		m_recv.ProcessPacket( cbPktSize );
		m_usecTimeLastRecv = usecNow;
		m_usecInFlightReplyTimeout = 0;
		m_nReplyTimeoutsSinceLastRecv = 0;
		m_usecWhenTimeoutStarted = 0;
	}

	/// Called when we receive a packet with a sequence number, to update estimated
	/// number of dropped packets, etc.  Returns the full 64-bit sequence number
	/// for the flow.
	void TrackRecvSequencedPacket( uint16 unWireSequenceNumber, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev );
	void TrackRecvSequencedPacketGap( int16 nGap, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev );

	//
	// Instantaneous stats
	//

	// Accumulators for current interval
	int m_nPktsRecvSequencedCurrentInterval; // packets successfully received containing a sequence number
	int m_nPktsRecvDroppedCurrentInterval; // packets assumed to be dropped in the current interval
	int m_nPktsRecvWeirdSequenceCurrentInterval; // any sequence number deviation other than a simple dropped packet.  (Most recent interval.)
	int m_usecMaxJitterCurrentInterval;

	// Instantaneous rates, calculated from most recent completed interval
	float m_flInPacketsDroppedPct;
	float m_flInPacketsWeirdSequencePct;
	int m_usecMaxJitterPreviousInterval;

	//
	// Lifetime stats
	//

	// Lifetime counters
	int64 m_nPktsRecvSequenced;
	int64 m_nPktsRecvDropped;
	int64 m_nPktsRecvOutOfOrder;
	int64 m_nPktsRecvDuplicate;
	int64 m_nPktsRecvSequenceNumberLurch; // sequence number had a really large discontinuity

	/// Lifetime quality statistics
	PercentileGenerator<uint8> m_qualitySample;

	/// Histogram of quality intervals
	int m_nQualityHistogram100;
	int m_nQualityHistogram99;
	int m_nQualityHistogram97;
	int m_nQualityHistogram95;
	int m_nQualityHistogram90;
	int m_nQualityHistogram75;
	int m_nQualityHistogram50;
	int m_nQualityHistogram1;
	int m_nQualityHistogramDead;

	// Histogram of incoming latency variance
	int m_nJitterHistogramNegligible; // <1ms
	int m_nJitterHistogram1; // 1--2ms
	int m_nJitterHistogram2; // 2--5ms
	int m_nJitterHistogram5; // 5--10ms
	int m_nJitterHistogram10; // 10--20ms
	int m_nJitterHistogram20; // 20ms or more

	//
	// Misc stats bookkeeping
	//

	/// Check if it's been long enough since the last time we sent a ping,
	/// and we'd like to try to sneak one in if possible.
	///
	/// Note that in general, tracer pings are the only kind of pings that the relay
	/// ever sends.  It assumes that the endpoints will take care of any keepalives,
	/// etc that need to happen, and the relay can merely observe this process and take
	/// note of the outcome.
	inline bool BReadyToSendTracerPing( SteamNetworkingMicroseconds usecNow ) const
	{
		return m_ping.m_usecTimeLastSentPingRequest + k_usecLinkStatsPingRequestInterval < usecNow;
	}

	/// Check if we appear to be timing out and need to send an "aggressive" ping, meaning send it right
	/// now, request that the reply not be delayed, and also request that the relay (if any) confirm its
	/// connectivity as well.
	inline bool BNeedToSendPingImmediate( SteamNetworkingMicroseconds usecNow ) const
	{
		return
			m_nReplyTimeoutsSinceLastRecv > 0 // We're timing out
			&& m_usecLastSendPacketExpectingImmediateReply+k_usecAggressivePingInterval < usecNow; // we haven't just recently sent an agressive ping.
	}

	/// Check if we should send a keepalive ping.  In this case we haven't heard from the peer in a while,
	/// but we don't have any reason to think there are any problems.
	inline bool BNeedToSendKeepalive( SteamNetworkingMicroseconds usecNow ) const
	{
		return
			m_usecInFlightReplyTimeout == 0 // not already tracking some other message for which we expect a reply (and which would confirm that the connection is alive)
			&& m_usecTimeLastRecv + k_usecKeepAliveInterval < usecNow; // haven't heard from the peer recently
	}

	/// Check if we have data worth sending, if we have a good
	/// opportunity (inline in a data packet) to do it.
	inline bool BReadyToSendStats( SteamNetworkingMicroseconds usecNow )
	{
		bool bResult = false;
		if ( m_pktNumInFlight == 0 && !m_bDisconnected )
		{
			if ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportInterval < usecNow && BCheckHaveDataToSendInstantaneous( usecNow ) )
				bResult = true ;
			if ( m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportInterval < usecNow && BCheckHaveDataToSendLifetime( usecNow ) )
				bResult = true;
		}

		return bResult;
	}

	/// Fill out message with everything we'd like to send.  We don't assume that we will
	/// actually send it.  (We might be looking for a good opportunity, and the data we want
	/// to send doesn't fit.)
	void PopulateMessage( CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow );

	/// Called when we send a packet for which we expect a reply and
	/// for which we expect to get latency info.
	/// This implies TrackSentMessageExpectingReply.
	void TrackSentPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{
		TrackSentMessageExpectingReply( usecNow, bAllowDelayedReply );
		m_ping.m_usecTimeLastSentPingRequest = usecNow;
	}

	/// Called when we send any message for which we expect some sort of reply.  (But maybe not an ack.)
	void TrackSentMessageExpectingReply( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );

	/// Called when we receive stats from remote host
	void ProcessMessage( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow );

	/// Received from remote host
	SteamDatagramLinkInstantaneousStats m_latestRemote;
	SteamNetworkingMicroseconds m_usecTimeRecvLatestRemote;
	SteamDatagramLinkLifetimeStats m_lifetimeRemote;
	SteamNetworkingMicroseconds m_usecTimeRecvLifetimeRemote;

	/// Local time when peer last acknowledged instantaneous stats.
	SteamNetworkingMicroseconds m_usecPeerAckedInstaneous;
	int64 m_pktNumInFlight;
	bool m_bInFlightInstantaneous;
	bool m_bInFlightLifetime;

	/// Number of sequenced packets received since we last sent instantaneous stats
	int m_nPktsRecvSeqSinceSentInstantaneous;

	/// Number of packets we have sent, since we last sent instantaneous stats
	int m_nPktsSentSinceSentInstantaneous;

	/// Local time when we last sent lifetime stats.
	SteamNetworkingMicroseconds m_usecPeerAckedLifetime;

	/// Number of sequenced packets received since we last sent lifetime stats
	int m_nPktsRecvSeqSinceSentLifetime;
	int m_nPktsSentSinceSentLifetime;

	/// We sent lifetime stats on this seq number, has not been acknowledged.  <0 if none
	//int m_seqnumUnackedSentLifetime;

	/// We received lifetime stats at this sequence number, and should ack it soon.  <0 if none
	//int m_seqnumPendingAckRecvTimelife;

	/// Time when the current interval started
	SteamNetworkingMicroseconds m_usecIntervalStart;

	//
	// Reply timeout
	//

	/// If we have a message in flight for which we expect a reply (possibly delayed)
	/// and we haven't heard ANYTHING back, then this is the time when we should
	/// declare a timeout (and increment m_nReplyTimeoutsSinceLastRecv)
	SteamNetworkingMicroseconds m_usecInFlightReplyTimeout;

	/// Time when we last sent some sort of packet for which we expect
	/// an immediate reply.  m_stats.m_ping and m_usecInFlightReplyTimeout both
	/// remember when we send requests that expect replies, but both include
	/// ones that we allow the reply to be delayed.  This timestamp only includes
	/// ones that we do not allow to be delayed.
	SteamNetworkingMicroseconds m_usecLastSendPacketExpectingImmediateReply;

	/// Number of consecutive times a reply from this guy has timed out, since
	/// the last time we got valid communication from him.  This is reset basically
	/// any time we get a packet from the peer.
	int m_nReplyTimeoutsSinceLastRecv;

	/// Time when the current timeout (if any) was first detected.  This is not
	/// the same thing as the time we last heard from them.  For a mostly idle
	/// connection, the keepalive interval is relatively sparse, and so we don't
	/// know if we didn't hear from them, was it because there was a problem,
	/// or just they had nothing to say.  This timestamp measures the time when
	/// we expected to heard something but didn't.
	SteamNetworkingMicroseconds m_usecWhenTimeoutStarted;

	//
	// Populate public interface structure
	//
	void GetLinkStats( SteamDatagramLinkStats &s, SteamNetworkingMicroseconds usecNow ) const;

	/// This is the only function we needed to make virtual.  To factor this one
	/// out is really awkward, and this isn't called very often anyway.
	virtual void GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const;

	inline void PeerAckedInstantaneous( SteamNetworkingMicroseconds usecNow )
	{
		m_usecPeerAckedInstaneous = usecNow;
		m_nPktsRecvSeqSinceSentInstantaneous = 0;
		m_nPktsSentSinceSentInstantaneous = 0;
	}
	inline void PeerAckedLifetime( SteamNetworkingMicroseconds usecNow )
	{
		m_usecPeerAckedLifetime = usecNow;
		m_nPktsRecvSeqSinceSentLifetime = 0;
		m_nPktsSentSinceSentLifetime = 0;
	}

	void InFlightPktAck( SteamNetworkingMicroseconds usecNow )
	{
		if ( m_bInFlightInstantaneous )
			PeerAckedInstantaneous( usecNow );
		if ( m_bInFlightLifetime )
			PeerAckedLifetime( usecNow );
		m_pktNumInFlight = 0;
		m_bInFlightInstantaneous = m_bInFlightLifetime = false;
	}

	void InFlightPktTimeout()
	{
		m_pktNumInFlight = 0;
		m_bInFlightInstantaneous = m_bInFlightLifetime = false;
	}

protected:
	// Make sure it's used as abstract base.  Note that we require you to call Init()
	// with a timestamp value, so the constructor is empty by default.
	inline LinkStatsTrackerBase() {}

	/// Initialize the stats tracking object
	/// We don't do this as a virtual function, since it's easy to factor the code
	/// where outside code will just call the derived class Init() version directly,
	/// and also give it a really specific name so we don't forget that this isn't doing any
	/// derived class work and call it internally.
	void InitInternal( SteamNetworkingMicroseconds usecNow );

	/// Check if it's time to update, and if so, do it.
	/// This is another one we don't implement as a virtual function,
	/// and this is called frequently so giving the optimizer a bit more
	/// visibility can'thurt.
	void ThinkInternal( SteamNetworkingMicroseconds usecNow );

	bool BNeedToSendStatsInternal( SteamNetworkingMicroseconds usecNow );
	void SetDisconnectedInternal( bool bFlag, SteamNetworkingMicroseconds usecNow );

	void GetInstantaneousStats( SteamDatagramLinkInstantaneousStats &s ) const;

	void TrackSentMessageExpectingSeqNumAckInternal( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{
		TrackSentPingRequest( usecNow, bAllowDelayedReply );
	}

	/// When the connection is terminated, we set this flag.  At that point we will no
	/// longer expect the peer to ack, or request to flush stats, etc.  (Although we
	/// might indicate that we need to send an ack.)
	bool m_bDisconnected;
private:

	bool BCheckHaveDataToSendInstantaneous( SteamNetworkingMicroseconds usecNow );
	bool BCheckHaveDataToSendLifetime( SteamNetworkingMicroseconds usecNow );

	/// Called to force interval to roll forward now
	void UpdateInterval( SteamNetworkingMicroseconds usecNow );

	void StartNextInterval( SteamNetworkingMicroseconds usecNow );
};

struct LinkStatsTrackerEndToEnd : public LinkStatsTrackerBase
{

	// LinkStatsTrackerBase "overrides"
	virtual void GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const OVERRIDE;

	/// Calculate retry timeout the sender will use
	SteamNetworkingMicroseconds CalcSenderRetryTimeout() const
	{
		if ( m_ping.m_nSmoothedPing < 0 )
			return k_nMillion;
		// 3 x RTT + max delay, plus some slop.
		// If the receiver hands on to it for the max duration and
		// our RTT is very low
		return m_ping.m_nSmoothedPing*3000 + ( k_usecMaxDataAckDelay + 10000 );
	}

	/// Time when the current interval started
	SteamNetworkingMicroseconds m_usecSpeedIntervalStart;

	/// TX Speed, should match CMsgSteamDatagramLinkLifetimeStats 
	int m_nTXSpeed; 
	int m_nTXSpeedMax; 
	PercentileGenerator<int> m_TXSpeedSample;
	int m_nTXSpeedHistogram16; // Speed at kb/s
	int m_nTXSpeedHistogram32; 
	int m_nTXSpeedHistogram64;
	int m_nTXSpeedHistogram128;
	int m_nTXSpeedHistogram256;
	int m_nTXSpeedHistogram512;
	int m_nTXSpeedHistogram1024;
	int m_nTXSpeedHistogramMax;

	/// RX Speed, should match CMsgSteamDatagramLinkLifetimeStats 
	int m_nRXSpeed;
	int m_nRXSpeedMax;
	PercentileGenerator<int> m_RXSpeedSample;
	int m_nRXSpeedHistogram16; // Speed at kb/s
	int m_nRXSpeedHistogram32; 
	int m_nRXSpeedHistogram64;
	int m_nRXSpeedHistogram128;
	int m_nRXSpeedHistogram256;
	int m_nRXSpeedHistogram512;
	int m_nRXSpeedHistogram1024;
	int m_nRXSpeedHistogramMax;

	/// Called when we get a speed sample
	void UpdateSpeeds( int nTXSpeed, int nRXSpeed );

	bool BNeedToSendStats( SteamNetworkingMicroseconds usecNow ) { return BNeedToSendStatsInternal( usecNow ); }

protected:
	void InitInternal( SteamNetworkingMicroseconds usecNow );
	void ThinkInternal( SteamNetworkingMicroseconds usecNow );

private:

	void UpdateSpeedInterval( SteamNetworkingMicroseconds usecNow );
	void StartNextSpeedInterval( SteamNetworkingMicroseconds usecNow );
};

// LinkStatsTracker is conceptually a "base class".  However, since we want to avoid
// runtime dispatch through virtual function tables, we've inverted this, so that the
// type-specific class is used as a template parameter and a base class.  Any "virtual
// functions" then can be overridden, and the compiler has full visibility and optimization
// opportunities
template <typename TLinkStatsTracker>
struct LinkStatsTracker : public TLinkStatsTracker
{

	// "Virtual functions" that we are "overriding" at compile time
	// by the template argument
	inline void Init( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected = false )
	{
		TLinkStatsTracker::InitInternal( usecNow );
		TLinkStatsTracker::SetDisconnectedInternal( bStartDisconnected, usecNow );
	}
	inline void Think( SteamNetworkingMicroseconds usecNow ) { TLinkStatsTracker::ThinkInternal( usecNow ); }
	inline void SetDisconnected( bool bFlag, SteamNetworkingMicroseconds usecNow ) { if ( TLinkStatsTracker::m_bDisconnected != bFlag ) TLinkStatsTracker::SetDisconnectedInternal( bFlag, usecNow ); }
	inline bool IsDisconnected() const { return TLinkStatsTracker::m_bDisconnected; }

	/// Called after we actually send connection data.  Note that we must have consumed the outgoing sequence
	/// for that packet (using GetNextSendSequenceNumber), but must *NOT* have consumed any more!
	void TrackSentStats( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{

		// Check if we expect our peer to know how to acknowledge this
		if ( !TLinkStatsTracker::m_bDisconnected )
		{
			TLinkStatsTracker::m_pktNumInFlight = TLinkStatsTracker::m_nNextSendSequenceNumber-1;
			TLinkStatsTracker::m_bInFlightInstantaneous = msg.has_instantaneous();
			TLinkStatsTracker::m_bInFlightLifetime = msg.has_lifetime();

			// They should ack.  Make a note of the sequence number that we used,
			// so that we can measure latency when they reply, setup timeout bookkeeping, etc
			TrackSentMessageExpectingSeqNumAck( usecNow, bAllowDelayedReply );
		}
		else
		{
			// Peer can't ack.  Just mark them as acking immediately
			Assert( TLinkStatsTracker::m_pktNumInFlight == 0 );
			TLinkStatsTracker::m_pktNumInFlight = 0;
			TLinkStatsTracker::m_bInFlightInstantaneous = false;
			TLinkStatsTracker::m_bInFlightLifetime = false;
			if ( msg.has_instantaneous() )
				TLinkStatsTracker::PeerAckedInstantaneous( usecNow );
			if ( msg.has_lifetime() )
				TLinkStatsTracker::PeerAckedLifetime( usecNow );
		}
	}

	/// Called after we send a packet for which we expect an ack.  Note that we must have consumed the outgoing sequence
	/// for that packet (using GetNextSendSequenceNumber), but must *NOT* have consumed any more!
	/// This call implies TrackSentPingRequest, since we will be able to match up the ack'd sequence
	/// number with the time sent to get a latency estimate.
	inline void TrackSentMessageExpectingSeqNumAck( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{
		TLinkStatsTracker::TrackSentMessageExpectingSeqNumAckInternal( usecNow, bAllowDelayedReply );
	}

	void RecvPktNumAckInternal( int64 nPktNum );
};


//
// Pack/unpack C struct <-> protobuf message
//
extern void LinkStatsInstantaneousStructToMsg( const SteamDatagramLinkInstantaneousStats &s, CMsgSteamDatagramLinkInstantaneousStats &msg );
extern void LinkStatsInstantaneousMsgToStruct( const CMsgSteamDatagramLinkInstantaneousStats &msg, SteamDatagramLinkInstantaneousStats &s );
extern void LinkStatsLifetimeStructToMsg( const SteamDatagramLinkLifetimeStats &s, CMsgSteamDatagramLinkLifetimeStats &msg );
extern void LinkStatsLifetimeMsgToStruct( const CMsgSteamDatagramLinkLifetimeStats &msg, SteamDatagramLinkLifetimeStats &s );

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKING_STATSUTILS_H
