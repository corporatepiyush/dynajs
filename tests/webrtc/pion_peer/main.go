// pion_peer -- interop peer for test_webrtc_pion.js, built on github.com/pion/webrtc (the Go WebRTC stack) as an
// independent implementation. Signaling is JSON over loopback UDP to the parent's port (same wire as
// webrtc_peer_child.js): {"t":"hello"}, {"t":"desc","d":{type,sdp}}, {"t":"cand","c":{candidate,sdpMid,sdpMLineIndex}|null}.
// Usage: pion_peer <parentPort> <offer|answer> <dtls-client|dtls-server|auto> [bench]
// PION_ZCSUM=1 enables RFC 9653 SCTP zero checksum (SettingEngine.EnableSCTPZeroChecksum).
// Every data-channel message is echoed back unchanged; "bye" closes that channel; the process exits once the peer
// connection is closed or failed. As offerer it opens "ordered", "unordered" (ordered:false) and "partial"
// (ordered:false, maxRetransmits:0).
package main

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"strconv"
	"sync"
	"time"

	"github.com/pion/webrtc/v4"
)

type msg struct {
	T string                     `json:"t"`
	D *webrtc.SessionDescription `json:"d,omitempty"`
	C *webrtc.ICECandidateInit   `json:"c,omitempty"`
}

func main() {
	port, _ := strconv.Atoi(os.Args[1])
	role := os.Args[2]
	dtls := os.Args[3]
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		panic(err)
	}
	parent := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}
	var wmu sync.Mutex
	send := func(m msg) {
		b, _ := json.Marshal(m)
		wmu.Lock()
		conn.WriteToUDP(b, parent)
		wmu.Unlock()
	}

	se := webrtc.SettingEngine{}
	se.SetIncludeLoopbackCandidate(true)
	se.SetIPFilter(func(ip net.IP) bool { return ip.IsLoopback() && ip.To4() != nil })
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	se.SetICEMulticastDNSMode(0)
	if os.Getenv("PION_ZCSUM") == "1" {
		se.EnableSCTPZeroChecksum(true)
	}
	switch dtls {
	case "dtls-client":
		se.SetAnsweringDTLSRole(webrtc.DTLSRoleClient)
	case "dtls-server":
		se.SetAnsweringDTLSRole(webrtc.DTLSRoleServer)
	}
	api := webrtc.NewAPI(webrtc.WithSettingEngine(se))
	pc, err := api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		panic(err)
	}
	done := make(chan struct{})
	var once sync.Once
	pc.OnConnectionStateChange(func(s webrtc.PeerConnectionState) {
		fmt.Fprintln(os.Stderr, "pion state", s.String())
		if s == webrtc.PeerConnectionStateClosed || s == webrtc.PeerConnectionStateFailed {
			once.Do(func() { close(done) })
		}
	})
	pc.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c == nil {
			send(msg{T: "cand"})
			return
		}
		ci := c.ToJSON()
		send(msg{T: "cand", C: &ci})
	})
	echo := func(dc *webrtc.DataChannel) {
		dc.OnMessage(func(m webrtc.DataChannelMessage) {
			if m.IsString && string(m.Data) == "bye" {
				dc.Close()
				return
			}
			if m.IsString {
				dc.SendText(string(m.Data))
			} else {
				dc.Send(m.Data)
			}
		})
	}
	pc.OnDataChannel(echo)
	var pending []webrtc.ICECandidateInit
	haveRemote := false
	var mu sync.Mutex
	go func() {
		buf := make([]byte, 1<<16)
		for {
			n, _, err := conn.ReadFromUDP(buf)
			if err != nil {
				return
			}
			var m msg
			if json.Unmarshal(buf[:n], &m) != nil {
				continue
			}
			switch m.T {
			case "desc":
				if err := pc.SetRemoteDescription(*m.D); err != nil {
					fmt.Fprintln(os.Stderr, "pion srd:", err)
					continue
				}
				mu.Lock()
				haveRemote = true
				for _, c := range pending {
					pc.AddICECandidate(c)
				}
				pending = nil
				mu.Unlock()
				if m.D.Type == webrtc.SDPTypeOffer {
					ans, err := pc.CreateAnswer(nil)
					if err != nil {
						fmt.Fprintln(os.Stderr, "pion answer:", err)
						continue
					}
					pc.SetLocalDescription(ans)
					send(msg{T: "desc", D: pc.LocalDescription()})
				}
			case "cand":
				if m.C == nil {
					continue
				}
				mu.Lock()
				if haveRemote {
					pc.AddICECandidate(*m.C)
				} else {
					pending = append(pending, *m.C)
				}
				mu.Unlock()
			case "close":
				pc.Close()
			}
		}
	}()
	send(msg{T: "hello"})
	if role == "offer" {
		f := false
		zero := uint16(0)
		for _, spec := range []struct {
			label string
			init  *webrtc.DataChannelInit
		}{
			{"ordered", nil},
			{"unordered", &webrtc.DataChannelInit{Ordered: &f}},
			{"partial", &webrtc.DataChannelInit{Ordered: &f, MaxRetransmits: &zero}},
		} {
			dc, err := pc.CreateDataChannel(spec.label, spec.init)
			if err != nil {
				panic(err)
			}
			echo(dc)
		}
		off, err := pc.CreateOffer(nil)
		if err != nil {
			panic(err)
		}
		pc.SetLocalDescription(off)
		send(msg{T: "desc", D: pc.LocalDescription()})
	}
	select {
	case <-done:
	case <-time.After(120 * time.Second):
	}
	pc.Close()
}
