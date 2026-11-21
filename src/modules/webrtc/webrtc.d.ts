/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:webrtc
/** dyna:webrtc — W3C WebRTC (webrtc-pc) peer connections and data channels, natively in C: ICE (RFC 8445, full agent,
 *  STUN RFC 8489, TURN-over-UDP RFC 8656 client), DTLS 1.2 through the linked OpenSSL (RFC 6347, use_srtp RFC 5764,
 *  certificate fingerprints RFC 8122), SCTP over DTLS (RFC 9260 / RFC 8261, partial reliability RFC 3758, stream reset
 *  RFC 6525) and DCEP (RFC 8832). The same class objects are installed as globals (globalThis.RTCPeerConnection ===
 *  import("dyna:webrtc").RTCPeerConnection), created on first use. Signaling is the caller's: exchange
 *  localDescription / icecandidate events over any channel. Media negotiation (webrtc-pc 5, JSEP RFC 8829): transceivers, senders, receivers,
 *  tracks and streams, offer/answer with audio and video m-sections in one BUNDLE group (RFC 9143) beside the data
 *  m-section; RTP/SRTP media flow is not wired yet. One transport per connection: an answer rejects (port 0) an
 *  accepted m-section outside the offer's BUNDLE group. A re-offer keeps every payload type and extmap id negotiated
 *  before (RFC 8829 5.2.2): a codec or extension the remote numbered keeps that number, and a default number the remote
 *  gave to something else moves to a free dynamic one (rtx apt follows). A simulcast offer (a=rid send +
 *  a=simulcast:send, RFC 8853) is answered with a=rid recv and a=simulcast:recv for the rids it lists when the answer
 *  receives; as the offerer, applying an answer removes the encodings whose rid its a=simulcast:recv omits and keeps
 *  only the first encoding when the answer has no simulcast. A remote offer asking us to send simulcast
 *  (a=simulcast:recv) is answered without simulcast. Rollback of a remote re-offer restores the remote stream
 *  membership of every receiver track and the direction, currentDirection and stopped state of a transceiver the offer
 *  stopped (what the application changed meanwhile is kept); a receiver track the offer ended stays ended (a track
 *  never returns to "live").
 *  Design choices: one UDP socket per gathered interface on the native batched UDP layer, each owning 32 receive buffers
 *  of 2 KiB (datagrams are processed in place) and 4 MiB kernel buffers, so a full 1 MiB SCTP window burst on loopback is
 *  not dropped; receiving never allocates per packet (fixed rings, one reassembly buffer per association, out-of-order
 *  chunks in recycled 1280-byte slots, at most 256 cached per association); send() copies each message once into the
 *  SCTP queue; a DATA chunk that fills a gap is SACKed at once (RFC 9260 6.7), so a reopened window is announced without
 *  the 200 ms delayed-SACK wait; a live RTCPeerConnection keeps the event loop alive until close() (or until the
 *  engine shuts down, which closes every connection); events are delivered as macrotasks after the operation that
 *  caused them has settled. */
declare module "dyna:webrtc" {
    type RTCSdpType = "offer" | "pranswer" | "answer" | "rollback";
    type RTCSignalingState = "stable" | "have-local-offer" | "have-remote-offer" | "have-local-pranswer" | "have-remote-pranswer" | "closed";
    type RTCIceGatheringState = "new" | "gathering" | "complete";
    type RTCRtpTransceiverDirection = "sendrecv" | "sendonly" | "recvonly" | "inactive" | "stopped";
    type RTCPriorityType = "very-low" | "low" | "medium" | "high";

    /** dyna extension, not part of webrtc-pc: the optional second constructor argument. A strict bag: an unknown key is a
     *  TypeError naming the valid ones; anything but an object is a TypeError. Spec behaviour never depends on it; it only
     *  moves the safety caps below. */
    interface RTCPeerConnectionDynaOptions {
        /** Caps applied while parsing every remote description and building every local one. Each value is a positive
         *  integer, or Infinity to remove the cap (it then stops only at 2^24 entries, 2^30 bytes for the two byte caps);
         *  a non-number is a TypeError, 0, a fraction or a value above that ceiling is a RangeError. A description over a
         *  cap is refused whole (OperationError, errorDetail "sdp-syntax-error"), never truncated.
         *  Defaults: maxInputBytes 1048576, maxLineBytes 4096, maxMediaSections 64, maxCodecsPerSection 128,
         *  maxHeaderExtensions 64 (per m= section), maxCandidatesPerSection 64, maxSsrcsPerSection 64,
         *  maxMsidsPerSection 16, maxSsrcGroups 16 (per m= section), maxSsrcsPerGroup 16, maxUnknownLines 1024 (per
         *  description). Raising them costs memory per description only: every parsed list is sized by what the
         *  description holds, not by the cap. */
        sdpLimits?: {
            maxInputBytes?: number;
            maxLineBytes?: number;
            maxMediaSections?: number;
            maxCodecsPerSection?: number;
            maxHeaderExtensions?: number;
            maxCandidatesPerSection?: number;
            maxSsrcsPerSection?: number;
            maxMsidsPerSection?: number;
            maxSsrcGroups?: number;
            maxSsrcsPerGroup?: number;
            maxUnknownLines?: number;
        };
    }

    /** A track handle (mediacapture-main), no capture devices: tracks come from RTCRtpReceiver.track or clone(), and
     *  carry no media in this module. new MediaStreamTrack() is a TypeError. A remote track starts muted; it is unmuted
     *  by nothing here (media flow is not wired), muted again when the remote side stops sending, and ended (with an
     *  "ended" event) when its transceiver is stopped by negotiation; stop() ends it without an event, a
     *  local transceiver stop() ends it at once and fires "ended" in a later task, close() ends it without an event. contentHint accepts "", "speech", "speech-recognition", "music" (audio) or "", "motion",
     *  "detail", "text" (video); other values are ignored. getCapabilities/getConstraints return {}, getSettings
     *  returns { deviceId }, applyConstraints resolves. */
    class MediaStreamTrack extends EventTarget {
        readonly kind: "audio" | "video";
        /** A UUID v4. */
        readonly id: string;
        readonly label: string;
        enabled: boolean;
        readonly muted: boolean;
        readonly readyState: "live" | "ended";
        contentHint: string;
        clone(): MediaStreamTrack;
        stop(): void;
        getCapabilities(): Record<string, unknown>;
        getConstraints(): Record<string, unknown>;
        getSettings(): Record<string, unknown>;
        applyConstraints(constraints?: Record<string, unknown>): Promise<void>;
        onended: ((ev: Event) => void) | null;
        onmute: ((ev: Event) => void) | null;
        onunmute: ((ev: Event) => void) | null;
    }

    /** mediacapture-main MediaStream: an ordered set of tracks (adding one twice is a no-op). addTrack/removeTrack fire
     *  nothing; addtrack/removetrack events ({ track }) fire only for remote streams, when a remote description adds a
     *  track to the stream (a=msid) or removes it. Remote streams are one object per stream id per connection. */
    class MediaStream extends EventTarget {
        constructor(init?: MediaStream | MediaStreamTrack[]);
        readonly id: string;
        /** True while any track is not ended. */
        readonly active: boolean;
        getTracks(): MediaStreamTrack[];
        getAudioTracks(): MediaStreamTrack[];
        getVideoTracks(): MediaStreamTrack[];
        getTrackById(id: string): MediaStreamTrack | null;
        addTrack(track: MediaStreamTrack): void;
        removeTrack(track: MediaStreamTrack): void;
        /** A new stream (new id) of clones of every track. */
        clone(): MediaStream;
        onaddtrack: ((ev: Event & { track: MediaStreamTrack }) => void) | null;
        onremovetrack: ((ev: Event & { track: MediaStreamTrack }) => void) | null;
    }

    interface RTCRtpCodec {
        mimeType: string;
        clockRate: number;
        channels?: number;
        sdpFmtpLine?: string;
    }
    interface RTCRtpCodecParameters extends RTCRtpCodec {
        payloadType: number;
    }
    /** The built-in table: audio opus/48000/2 (minptime=10;useinbandfec=1), PCMU, PCMA, telephone-event/48000 and
     *  telephone-event/8000 (RFC 4733, fmtp 0-15); video VP8, H264 profile-level-id
     *  42e01f and 42001f (packetization-mode=1), each with RTX (listed once as "video/rtx", no fmtp); header extensions
     *  sdes:mid and transport-wide-cc (both kinds), ssrc-audio-level (audio), rtp-stream-id and repaired-rtp-stream-id
     *  (video). */
    interface RTCRtpCapabilities {
        codecs: RTCRtpCodec[];
        headerExtensions: { uri: string }[];
    }
    interface RTCRtpEncodingParameters {
        /** RFC 8851 rid: 1..255 of [A-Za-z0-9_-]; read-only after addTransceiver. */
        rid?: string;
        active?: boolean;
        maxBitrate?: number;
        /** Video only (dropped for audio); >= 0, else RangeError. */
        maxFramerate?: number;
        /** Video only (dropped for audio); >= 1, else RangeError. Default 2^(n-i-1) for encoding i of n when no encoding
         *  sets it, 1 otherwise. */
        scaleResolutionDownBy?: number;
        priority?: RTCPriorityType;
        networkPriority?: RTCPriorityType;
        /** The codec to send this encoding with (webrtc-pc 5.2). addTransceiver: one not in
         *  RTCRtpSender.getCapabilities(kind).codecs is an OperationError. setParameters: it must match a negotiated
         *  codec, else (before negotiation) a codec preference other than RTX/RED/FEC, else a capability; otherwise
         *  InvalidModificationError. Applying an answer removes it when the answer did not keep that codec. */
        codec?: RTCRtpCodec;
    }
    interface RTCRtpParameters {
        /** The negotiated header extensions (empty until an answer is applied). */
        headerExtensions: { uri: string; id: number; encrypted: boolean }[];
        rtcp: { cname?: string; reducedSize?: boolean };
        /** The negotiated codecs (empty until an answer is applied). */
        codecs: RTCRtpCodecParameters[];
    }
    interface RTCRtpSendParameters extends RTCRtpParameters {
        transactionId: string;
        encodings: RTCRtpEncodingParameters[];
    }
    interface RTCRtpTransceiverInit {
        /** "stopped" is a TypeError. */
        direction?: RTCRtpTransceiverDirection;
        streams?: MediaStream[];
        /** Validated in order: a rid that breaks RFC 8851, a duplicate rid, or some-but-not-all encodings with a rid is a
         *  TypeError; maxFramerate < 0 or scaleResolutionDownBy < 1 (video) is a RangeError. Then the list is cut to 1
         *  encoding (audio) or 4 (video); a single remaining encoding loses its rid. Two or more video encodings offer
         *  a=rid send lines and a=simulcast:send. */
        sendEncodings?: RTCRtpEncodingParameters[];
    }

    /** webrtc-pc 5.2. getParameters() returns a transactionId that stays the same within one task and is cleared by the
     *  next macrotask (or by setParameters); setParameters() rejects, in this order: TypeError for a missing
     *  transactionId, encodings, headerExtensions, rtcp or codecs; InvalidStateError when the transceiver is stopping or
     *  there was no getParameters() in this task; InvalidModificationError for another transactionId, a changed number
     *  of encodings, a changed rid, or changed codecs, headerExtensions or rtcp; RangeError for scaleResolutionDownBy < 1
     *  or maxFramerate < 0. Encoding values are stored and reported back, not yet applied to media (media flow is not
     *  part of this module). */
    class RTCRtpSender {
        readonly track: MediaStreamTrack | null;
        /** null until the transceiver is associated with an m= section (setLocalDescription / setRemoteDescription),
         *  then the connection's one RTCDtlsTransport (everything is bundled), the same object for every sender,
         *  receiver and sctp.transport. */
        readonly transport: RTCDtlsTransport | null;
        /** An RTCDTMFSender for an audio sender (the same object each time), null for video. */
        readonly dtmf: RTCDTMFSender | null;
        static getCapabilities(kind: string): RTCRtpCapabilities | null;
        getParameters(): RTCRtpSendParameters;
        setParameters(parameters: RTCRtpSendParameters): Promise<void>;
        /** Chained on the connection's operations queue. A track of another kind rejects with TypeError, a stopping
         *  transceiver with InvalidStateError; needs no renegotiation. A remote track (an RTCRtpReceiver's,
         *  from this or another connection) given to addTrack / addTransceiver / replaceTrack is relayed: every packet its receiver accepts
         *  is sent on this sender with this sender's SSRC, sequence and timestamp base, the payload type mapped by
         *  codec name, clock rate and channels to this m= section (packets with no match are skipped), CSRCs and the
         *  RFC 6464 / 6465 level bytes kept. The mapping follows renegotiation on either side. A tone from this
         *  sender's RTCDTMFSender takes the next sequence numbers and relaying continues after it (the tone keeps its
         *  own wall-clock RTP timestamp). Relaying stops on replaceTrack(null) or another track and when the relayed
         *  track ends. */
        replaceTrack(track: MediaStreamTrack | null): Promise<void>;
        /** Replaces the stream ids signalled in a=msid; fires negotiationneeded. Non-MediaStream arguments are a
         *  TypeError; a closed connection an InvalidStateError. */
        setStreams(...streams: MediaStream[]): void;
        /** Resolves an empty Map (RTP statistics come with media flow). */
        getStats(): Promise<Map<string, Record<string, unknown>>>;
    }

    class RTCRtpReceiver {
        /** Created with the transceiver; never null. */
        readonly track: MediaStreamTrack;
        /** Same object as RTCRtpSender.transport. */
        readonly transport: RTCDtlsTransport | null;
        /** ms, 0..4000 (RangeError outside), or null for the default. */
        jitterBufferTarget: number | null;
        static getCapabilities(kind: string): RTCRtpCapabilities | null;
        getParameters(): RTCRtpParameters;
        /** One entry per CSRC (getContributingSources) or SSRC (getSynchronizationSources) seen in the last 10 s,
         *  newest first (webrtc-pc 5.3). A packet counts once it passes SRTP authentication and BUNDLE routing (MID
         *  header extension, then a signalled or latched SSRC, then a payload type unique to one receiver, RFC 8843
         *  9.2); nothing is decoded before P3, so that is when it is "delivered to the track". An older RTP timestamp
         *  never overwrites a newer one. timestamp is wall-clock ms (comparable to Date.now(); dynajs has no
         *  performance.timeOrigin). audioLevel only for audio, from RFC 6464 (SSRC) / RFC 6465 (CSRC; offered as
         *  urn:ietf:params:rtp-hdrext:csrc-audio-level), 127 -> 0, else 10^(-level/20); absent without the value. Up to
         *  4 SSRCs and 16 CSRCs are kept per receiver (the oldest is replaced). The remote track starts muted, is
         *  unmuted (a task firing "unmute") by the first such packet while the transceiver is receptive (local
         *  direction includes recv), and muted again by an RTCP BYE for its SSRC or after 25 s without RTP or an RTCP sender report from
         *  its source (RFC 3550 6.3.5, 5 x the 5 s minimum report interval); the next packet unmutes it again.
         *  [[Receptive]] is set by setLocalDescription from the direction in the description's m= line (fixed when the
         *  offer or answer was created), not from transceiver.direction at that moment. */
        getContributingSources(): { timestamp: number; source: number; audioLevel?: number; rtpTimestamp: number }[];
        getSynchronizationSources(): { timestamp: number; source: number; audioLevel?: number; rtpTimestamp: number }[];
        getStats(): Promise<Map<string, Record<string, unknown>>>;
    }

    /** webrtc-pc 5.4. Setting direction to an unknown value is ignored (WebIDL enum attribute), to "stopped" is a
     *  TypeError, on a stopping transceiver an InvalidStateError; a change fires negotiationneeded. stop() makes it
     *  stopping (direction "stopped"; the receiver track is ended at once and its "ended" event fires in a later task,
     *  webrtc-pc 5.4 stop sending and receiving); the next negotiation rejects its m= section
     *  (port 0), after which currentDirection is "stopped" and the transceiver
     *  leaves getTransceivers() once the signaling state returns to stable. stop() on a closed connection is an
     *  InvalidStateError. A transceiver stopped by the remote side (its m= section rejected) ends its
     *  receiver track with an "ended" event. */
    class RTCRtpTransceiver {
        readonly mid: string | null;
        readonly sender: RTCRtpSender;
        readonly receiver: RTCRtpReceiver;
        direction: RTCRtpTransceiverDirection;
        readonly currentDirection: RTCRtpTransceiverDirection | null;
        stop(): void;
        /** Orders and filters the codecs of the next offer or answer for this m= section. Each entry must match one of
         *  RTCRtpReceiver.getCapabilities(kind).codecs (mimeType case-insensitive, clockRate, channels, sdpFmtpLine)
         *  else InvalidModificationError; a list of only rtx/red/ulpfec/flexfec/CN is an InvalidModificationError;
         *  listing video/rtx keeps the RTX of every listed codec; [] restores the default order. */
        setCodecPreferences(codecs: RTCRtpCodec[]): void;
    }

    /** webrtc-pc 5.6. The connection's one ICE transport (component "rtp"; everything is bundled). state is the
     *  connection's iceConnectionState, gatheringState its iceGatheringState; role is "unknown" before ICE starts, then
     *  "controlling" (offerer, or the full agent facing ice-lite) or "controlled" (RFC 8445 6.1.1). The candidate lists
     *  are RTCIceCandidates built from the gathered / received candidates; getSelectedCandidatePair() is null until a
     *  pair is selected and after close(). getLocalParameters / getRemoteParameters are the a=ice-ufrag / a=ice-pwd
     *  in use (null before they are known). Events: statechange, gatheringstatechange, selectedcandidatepairchange.
     *  close() sets state "closed" without an event. Not constructible (TypeError). */
    class RTCIceTransport extends EventTarget {
        readonly role: "unknown" | "controlling" | "controlled";
        readonly component: "rtp";
        readonly state: RTCIceConnectionState;
        readonly gatheringState: RTCIceGatheringState;
        getLocalCandidates(): RTCIceCandidate[];
        getRemoteCandidates(): RTCIceCandidate[];
        getSelectedCandidatePair(): { local: RTCIceCandidate; remote: RTCIceCandidate } | null;
        getLocalParameters(): { usernameFragment: string; password: string } | null;
        getRemoteParameters(): { usernameFragment: string; password: string } | null;
        onstatechange: ((ev: Event) => void) | null;
        ongatheringstatechange: ((ev: Event) => void) | null;
        onselectedcandidatepairchange: ((ev: Event) => void) | null;
    }

    /** webrtc-pc 5.5. The connection's one DTLS transport. state: "new" -> "connecting" -> "connected", or "failed"
     *  (an "error" event fires first with an RTCError, a DOMException OperationError whose errorDetail is
     *  "fingerprint-failure" when the peer certificate does not match the signalled a=fingerprint, else "dtls-failure"),
     *  or "closed" (close_notify received, or close() without an event). getRemoteCertificates() is [the peer's DER
     *  certificate as a new ArrayBuffer] while connected, else []. Not constructible (TypeError). */
    class RTCDtlsTransport extends EventTarget {
        readonly iceTransport: RTCIceTransport;
        readonly state: "new" | "connecting" | "connected" | "closed" | "failed";
        getRemoteCertificates(): ArrayBuffer[];
        onstatechange: ((ev: Event) => void) | null;
        onerror: ((ev: Event & { error: DOMException & { errorDetail: string } }) => void) | null;
    }

    /** webrtc-pc 7. canInsertDTMF is true only when the connection is "connected", the transceiver is not stopping, the
     *  sender has a track, currentDirection is sendrecv or sendonly, the first encoding is active and
     *  audio/telephone-event (RFC 4733) was negotiated. insertDTMF(tones, duration = 100, interToneGap = 70): when
     *  canInsertDTMF is false an InvalidStateError (checked before the characters); a character outside 0-9 A-D a-d # *
     *  ',' is an InvalidCharacterError; a-d become A-D; duration clamps to 40..6000 ms and interToneGap to 30..6000 ms;
     *  the call replaces toneBuffer and starts the playout task only if none is scheduled. Each task run removes one
     *  tone, schedules the next run after duration + interToneGap (2000 ms after ','), then fires tonechange with that
     *  tone; an empty buffer fires tonechange with tone "" and stops; once DTMF can no longer be sent the task stops
     *  without an event, leaving the rest in toneBuffer. Each tone (not ',') goes on the wire as RFC 4733 telephone-event
     *  packets on the negotiated telephone-event payload type whose clock matches the audio codec (else the first
     *  one): event 0-9 * # A-D = 0-15, volume 10, one RTP timestamp per tone, marker on the first packet, one update
     *  every 50 ms carrying the elapsed duration plus 50 ms (through the next update, never past the tone), then three
     *  end packets (E bit) with the full duration; a tone longer than 65535 clock units is split into segments (new
     *  timestamp, no marker).
     *  Not constructible (TypeError). */
    class RTCDTMFSender extends EventTarget {
        insertDTMF(tones: string, duration?: number, interToneGap?: number): void;
        readonly canInsertDTMF: boolean;
        readonly toneBuffer: string;
        ontonechange: ((ev: Event & { tone: string }) => void) | null;
    }

    interface RTCTrackEvent extends Event {
        readonly receiver: RTCRtpReceiver;
        readonly track: MediaStreamTrack;
        readonly streams: MediaStream[];
        readonly transceiver: RTCRtpTransceiver;
    }

    type RTCIceConnectionState = "new" | "checking" | "connected" | "completed" | "disconnected" | "failed" | "closed";
    type RTCPeerConnectionState = "new" | "connecting" | "connected" | "disconnected" | "failed" | "closed";
    type RTCDataChannelState = "connecting" | "open" | "closing" | "closed";
    type RTCIceCandidateType = "host" | "srflx" | "prflx" | "relay";

    /** One STUN or TURN server. urls: "stun:host[:port]" or "turn:host[:port][?transport=udp|tcp]"; validated by the W3C
     *  "validate an ICE server URL" steps (SyntaxError DOMException for a malformed URL or empty list, NotSupportedError for
     *  stuns:/turns:, InvalidAccessError for a turn: URL without username and credential). A turn: URL with
     *  ?transport=tcp is accepted and not used for gathering (TURN is used over UDP only). Host names resolve once at
     *  construction; a server no local socket can reach fires icecandidateerror 701 when gathering starts. */
    interface RTCIceServer {
        urls: string | string[];
        username?: string;
        credential?: string;
    }

    interface RTCConfiguration {
        iceServers?: RTCIceServer[];
        /** "relay" gathers relay candidates only (host and srflx are never offered nor used). */
        iceTransportPolicy?: "all" | "relay";
        /** Any value works the same here (the single data m-section is always bundled); getConfiguration reports it
         *  (default "balanced") and setConfiguration cannot change it. */
        bundlePolicy?: "balanced" | "max-compat" | "max-bundle";
        /** Only "require" is accepted (rtcp-mux is mandatory; "negotiate" is a NotSupportedError per webrtc-pc). */
        rtcpMuxPolicy?: "require";
        /** Certificates from RTCPeerConnection.generateCertificate(); default: a fresh ECDSA P-256 certificate. An expired
         *  one is an InvalidAccessError. The default one is generated at the first createOffer, createAnswer or implicit setLocalDescription() on the CPU
         *  offload pool, inside the operations chain: that operation and the ones queued after it wait for it without
         *  blocking the event loop, and a close() meanwhile leaves them unsettled; under --io-threads 0 it is generated
         *  inline. A keygen still running at exit is dropped by the shutdown sweep. */
        certificates?: RTCCertificate[];
        iceCandidatePoolSize?: number;
        /** dynajs extension: the IP literals (at most 8) to gather host candidates on, instead of every up interface.
         *  The default gathers every up interface except loopback, link-local, IPv6 site-local (fec0::/10), IPv4-compatible
         *  and IPv4-mapped IPv6 (RFC 8445 5.1.1.1); with none left there are no host candidates. Loopback is used only when
         *  listed here (tests). Not a webrtc-pc member; browsers ignore it. */
        iceLocalAddresses?: string[];
        /** dynajs extension for tests and constrained peers: the SCTP inbound/outbound stream counts advertised in
         *  INIT/INIT_ACK (1..65535). The default is 65535, per RFC 8831 6.2. maxChannels is the minimum of the negotiated
         *  counts in both directions. Not a webrtc-pc member; browsers ignore it. */
        sctpStreams?: number;
        /** dynajs extension for tests: false stops advertising RFC 9653 zero checksum (default true). */
        sctpZeroChecksum?: boolean;
    }

    interface RTCSessionDescriptionInit {
        type: RTCSdpType;
        sdp?: string;
    }

    interface RTCIceCandidateInit {
        candidate?: string;
        sdpMid?: string | null;
        sdpMLineIndex?: number | null;
        usernameFragment?: string | null;
    }

    interface RTCDataChannelInit {
        ordered?: boolean;
        /** At most one of maxPacketLifeTime (ms) and maxRetransmits (TypeError when both). Values clamp to 65535. */
        maxPacketLifeTime?: number;
        maxRetransmits?: number;
        /** At most 65535 UTF-8 bytes, as is label. */
        protocol?: string;
        /** true: no DCEP open; both sides create the channel with the same id (required then, 0..65534). */
        negotiated?: boolean;
        id?: number;
        priority?: "very-low" | "low" | "medium" | "high";
    }

    interface RTCOfferOptions {
        iceRestart?: boolean;
    }

    /** RTCSctpTransport as a plain object. maxMessageSize = min(local limit, remote a=max-message-size, where 0 means no
     *  limit); the local limit is 16 MiB and is what this side advertises. maxChannels is null until connected. */
    interface RTCSctpTransportInfo {
        /** The connection's RTCDtlsTransport. */
        readonly transport: RTCDtlsTransport;
        readonly state: "connecting" | "connected" | "closed";
        readonly maxMessageSize: number;
        readonly maxChannels: number | null;
    }

    interface RTCDataChannelEvent extends Event {
        readonly channel: RTCDataChannel;
    }
    interface RTCPeerConnectionIceEvent extends Event {
        /** null marks the end of gathering for this generation. */
        readonly candidate: RTCIceCandidate | null;
        readonly url: string | null;
    }
    /** errorCode: the STUN error code from the server (401, 437, ...), or 701 when the server could not be reached
     *  (RFC 8489 6.2.1 retransmission schedule exhausted: 7 requests, 39.5 s). */
    interface RTCPeerConnectionIceErrorEvent extends Event {
        readonly address: string | null;
        readonly port: number | null;
        readonly url: string;
        readonly errorCode: number;
        readonly errorText: string;
    }
    interface MessageEvent extends Event {
        /** string for a text message; ArrayBuffer or Blob (by binaryType) for a binary one. */
        readonly data: string | ArrayBuffer | Blob;
    }

    class RTCSessionDescription {
        constructor(init: RTCSessionDescriptionInit);
        readonly type: RTCSdpType;
        readonly sdp: string;
        toJSON(): RTCSessionDescriptionInit;
    }

    /** Parses the candidate attribute (RFC 8839 5.1) at construction; a malformed one is a TypeError only when sdpMid and
     *  sdpMLineIndex are both null. Getters expose the parsed fields (null when absent). */
    class RTCIceCandidate {
        constructor(init?: RTCIceCandidateInit);
        readonly candidate: string;
        readonly sdpMid: string | null;
        readonly sdpMLineIndex: number | null;
        readonly foundation: string | null;
        readonly component: "rtp" | "rtcp" | null;
        readonly priority: number | null;
        readonly address: string | null;
        readonly protocol: "udp" | "tcp" | null;
        readonly port: number | null;
        readonly type: RTCIceCandidateType | null;
        readonly tcpType: string | null;
        readonly relatedAddress: string | null;
        readonly relatedPort: number | null;
        readonly usernameFragment: string | null;
        readonly relayProtocol: "udp" | null;
        readonly url: string | null;
        toJSON(): RTCIceCandidateInit;
    }

    class RTCCertificate {
        /** ms since the epoch. */
        readonly expires: number;
        getFingerprints(): { algorithm: "sha-256"; value: string }[];
    }

    /** A data channel. send() accepts string (sent as UTF-8, PPID 51, or 56 when empty), ArrayBuffer, any ArrayBufferView
     *  or Blob (PPID 53, or 57 when empty); it throws InvalidStateError unless readyState is "open" and TypeError when the
     *  byte length exceeds pc.sctp.maxMessageSize. Messages larger than one SCTP chunk are fragmented and reassembled
     *  (RFC 9260 6.9). bufferedAmount counts user bytes queued and not yet acknowledged, never DCEP or padding bytes. close()
     *  resets the stream (RFC 8831 6.7): "closing", then "close" once both directions are reset; the channel keeps its JS
     *  object alive until then. */
    class RTCDataChannel extends EventTarget {
        readonly label: string;
        readonly ordered: boolean;
        readonly maxPacketLifeTime: number | null;
        readonly maxRetransmits: number | null;
        readonly protocol: string;
        readonly negotiated: boolean;
        readonly id: number | null;
        readonly readyState: RTCDataChannelState;
        readonly bufferedAmount: number;
        bufferedAmountLowThreshold: number;
        /** "blob" (default, per webrtc-pc) or "arraybuffer". */
        binaryType: "blob" | "arraybuffer";
        readonly priority: "very-low" | "low" | "medium" | "high";
        onopen: ((ev: Event) => void) | null;
        onmessage: ((ev: MessageEvent) => void) | null;
        onbufferedamountlow: ((ev: Event) => void) | null;
        onerror: ((ev: Event) => void) | null;
        onclosing: ((ev: Event) => void) | null;
        onclose: ((ev: Event) => void) | null;
        send(data: string | ArrayBuffer | ArrayBufferView | Blob): void;
        close(): void;
    }

    /** One peer connection. Operations (createOffer, createAnswer, setLocalDescription, setRemoteDescription,
     *  addIceCandidate) run one at a time in call order (the webrtc-pc operations chain) and reject with
     *  InvalidStateError once closed. The offerer is ICE-controlling (unless the answer is ice-lite) and the DTLS server
     *  (a=setup:actpass; the answer says active), so its data-channel ids are odd and the answerer's even (RFC 8832 6).
     *  Trickle ICE: candidates arrive as icecandidate events after setLocalDescription settles; localDescription also
     *  lists every candidate gathered so far, so a non-trickle peer can wait for iceGatheringState "complete".
     *  getStats() resolves an RTCStatsReport (a Map) with peer-connection, data-channel, transport, sctp-transport,
     *  local-candidate, remote-candidate, candidate-pair and certificate entries, also after close(). The sctp-transport
     *  entry adds dynajs fields forwardTsnSent, initChunksSent, zeroChecksumNegotiated, zeroChecksumPacketsSent and
     *  zeroChecksumPacketsReceived; the transport entry adds receiveWakeups (event drains, one per socket wakeup that
     *  delivered datagrams, so it stays below packetsReceived under bursts). SCTP zero checksum (RFC 9653, EDMID 1 = SCTP over DTLS) is advertised in INIT and
     *  INIT ACK; packets go out with a zero checksum only after the peer advertised it and only in established/shutdown
     *  states (INIT, INIT ACK and COOKIE ECHO always carry CRC32c); a zero checksum is accepted only when we advertised
     *  it, never on INIT or COOKIE ECHO, never before our INIT/INIT ACK went out; a correct CRC32c is always accepted.
     *  Threading: one RTCPeerConnection, its channels, ICE, DTLS and SCTP state live on the event loop of the realm that
     *  created it; the packet path (UDP receive, STUN, DTLS records, SCTP, DCEP, events) runs on that loop thread with no
     *  locks, and neither the connection nor a channel can be posted to another Worker (postMessage
     *  throws). generateCertificate generates the key on the shared io pool and settles on the loop; with
     *  --io-threads 0 it runs inline on the loop thread.
     *  SCTP: both endpoints send INIT once DTLS connects (RFC 8841 10.2 lets either side start; a simultaneous open is
     *  the RFC 9260 5.2.1 INIT collision). An idle association sends HEARTBEAT every RTO + 30 s (RFC 9260 8.3); each
     *  unanswered one doubles RTO and counts toward Association.Max.Retrans (10), after which the association aborts
     *  (about 12 minutes for an idle peer that vanished; the SCTP layer test measures it), so a silent dead peer is
     *  detected even when nothing is sent; an answered HEARTBEAT clears the error count. */
    class RTCPeerConnection extends EventTarget {
        /** dynaOptions is a dyna extension (see RTCPeerConnectionDynaOptions), validated after the configuration; an invalid one throws and no connection is made. */
        constructor(configuration?: RTCConfiguration, dynaOptions?: RTCPeerConnectionDynaOptions);
        /** keygenAlgorithm: { name: "ECDSA", namedCurve: "P-256" } or { name: "RSASSA-PKCS1-v1_5", modulusLength
         *  1024..8192, publicExponent 65537, hash: "SHA-256" }, optionally with expires (ms, capped at 365 days). Anything
         *  else rejects with NotSupportedError. */
        static generateCertificate(keygenAlgorithm: string | object): Promise<RTCCertificate>;
        createOffer(options?: RTCOfferOptions): Promise<RTCSessionDescriptionInit>;
        createAnswer(): Promise<RTCSessionDescriptionInit>;
        /** With no argument: the implicit offer or answer for the current signaling state. */
        setLocalDescription(description?: RTCSessionDescriptionInit): Promise<void>;
        /** Validates the whole description before changing anything, so a rejected one leaves every state as it was
         *  (InvalidAccessError for a missing ice-ufrag/ice-pwd, fingerprint, an answer that does not match the offer or
         *  says actpass, or an answer that changes the ICE credentials of an offer that did not restart ICE;
         *  OperationError with errorDetail "sdp-syntax-error" for unparsable SDP); a valid offer in have-local-offer
         *  rolls the local offer back first. An ICE restart (either side) takes effect when its answer is applied: until
         *  then the current credentials, candidates and selected pair stay in use, and a rollback (explicit or by glare)
         *  drops it, after which the next offer restarts again if restartIce() asked for it. */
        setRemoteDescription(description: RTCSessionDescriptionInit): Promise<void>;
        /** null or { candidate: "" } marks end-of-candidates. */
        addIceCandidate(candidate?: RTCIceCandidateInit | null): Promise<void>;
        getStats(): Promise<Map<string, Record<string, unknown>>>;
        createDataChannel(label: string, init?: RTCDataChannelInit): RTCDataChannel;
        /** Closes every channel and the transports at once (DTLS close_notify, TURN allocations refreshed to lifetime 0);
         *  fires no state events (webrtc-pc). Operations queued before close() (createOffer, setRemoteDescription, ...)
         *  never settle, nor does one whose own events (signalingstatechange) called close(); calls made after it reject
         *  with InvalidStateError, except getStats. */
        close(): void;
        /** The next offer carries fresh ICE credentials; fires negotiationneeded. The ICE role is set by the initial
         *  offer/answer and an ICE restart never redetermines it (RFC 8445 6.1.1); only a peer turning ICE-lite or a role
         *  conflict (487) changes it. */
        restartIce(): void;
        getConfiguration(): RTCConfiguration;
        /** Changing certificates or bundlePolicy (an omitted one means "balanced") is an InvalidModificationError; an
         *  unknown bundlePolicy or rtcpMuxPolicy is a TypeError; an invalid configuration throws and changes nothing.
         *  getConfiguration reports the new iceTransportPolicy at once, but gathering, signalling and pairing follow it
         *  only from the next gathering.
         *  iceServers (omitted = none), iceTransportPolicy (omitted = "all") and iceLocalAddresses (omitted = kept) apply
         *  from the next gathering, the ICE restart: it opens sockets on interfaces that appeared, starts STUN/TURN for
         *  the new servers, and under "relay" signals and pairs only relay candidates; candidates already gathered and
         *  the selected pair stay in use until the restart selects a new pair. A server listed again with the same url,
         *  username and credential keeps its allocation (no second Allocate); switching "relay" back to "all" signals
         *  the host and srflx candidates of the existing sockets again; under "relay" the local description lists
         *  only relay candidates. A server no longer listed stops being signalled
         *  and paired at the restart, keeps carrying the selected pair, and is released (TURN Refresh LIFETIME 0)
         *  when the restart selects a pair on another session, so a relayed connection never loses its path while
         *  moving between servers; a pair the peer selects on it before then (renomination) keeps it alive until the
         *  next selection. A connection holds at most 64 live STUN/TURN sessions (servers x sockets); a released
         *  session frees its slot for the next one, sessions not yet released still count, and a server that finds
         *  no free slot gets an icecandidateerror with errorCode 701 and errorText "STUN/TURN session limit (64)
         *  reached" instead of being skipped silently. */
        setConfiguration(configuration: RTCConfiguration): void;
        /** Senders and receivers of transceivers that are not stopped; transceivers in creation order, stopped ones until
         *  they are removed (webrtc-pc 4.4.1.6). close() stops every transceiver without events and keeps them listed. */
        getSenders(): RTCRtpSender[];
        getReceivers(): RTCRtpReceiver[];
        getTransceivers(): RTCRtpTransceiver[];
        /** Reuses a transceiver of the same kind that never sent, has no track and is not stopping (recvonly becomes
         *  sendrecv, inactive becomes sendonly); otherwise creates a sendrecv one. TypeError for a non-track or non-stream
         *  argument, InvalidAccessError when the track already has a sender here, InvalidStateError when closed. */
        addTrack(track: MediaStreamTrack, ...streams: MediaStream[]): RTCRtpSender;
        /** sendrecv becomes recvonly, sendonly becomes inactive; a sender of another connection is an InvalidAccessError. */
        removeTrack(sender: RTCRtpSender): void;
        addTransceiver(trackOrKind: MediaStreamTrack | "audio" | "video", init?: RTCRtpTransceiverInit): RTCRtpTransceiver;
        readonly localDescription: RTCSessionDescription | null;
        readonly currentLocalDescription: RTCSessionDescription | null;
        readonly pendingLocalDescription: RTCSessionDescription | null;
        readonly remoteDescription: RTCSessionDescription | null;
        readonly currentRemoteDescription: RTCSessionDescription | null;
        readonly pendingRemoteDescription: RTCSessionDescription | null;
        readonly signalingState: RTCSignalingState;
        readonly iceGatheringState: RTCIceGatheringState;
        readonly iceConnectionState: RTCIceConnectionState;
        readonly connectionState: RTCPeerConnectionState;
        readonly canTrickleIceCandidates: boolean | null;
        /** null until a description with an application m-section is applied. */
        readonly sctp: RTCSctpTransportInfo | null;
        onicecandidate: ((ev: RTCPeerConnectionIceEvent) => void) | null;
        onicecandidateerror: ((ev: RTCPeerConnectionIceErrorEvent) => void) | null;
        onsignalingstatechange: ((ev: Event) => void) | null;
        oniceconnectionstatechange: ((ev: Event) => void) | null;
        onicegatheringstatechange: ((ev: Event) => void) | null;
        onconnectionstatechange: ((ev: Event) => void) | null;
        ondatachannel: ((ev: RTCDataChannelEvent) => void) | null;
        onnegotiationneeded: ((ev: Event) => void) | null;
        /** Fires inside setRemoteDescription, before its promise resolves (webrtc-pc 4.4.1.6: mute, then removetrack,
         *  then addtrack, then track events), once per transceiver that starts receiving: an offer associates its
         *  m-sections by mid, then with an unassociated addTrack transceiver of the same kind, else creates a recvonly
         *  one. A rollback removes the transceivers the remote offer created. */
        ontrack: ((ev: RTCTrackEvent) => void) | null;
    }
}
