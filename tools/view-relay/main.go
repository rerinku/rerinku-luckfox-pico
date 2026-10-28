// view-relay lets devices on this LAN (the Luckfox board) push to a Rerinku
// View they cannot reach directly: one only reachable over Tailscale. Run it
// on a machine that is on both networks; point the device's endpoint here.
//
//   - HTTP (registration, WHIP signaling, snapshots) is reverse-proxied to
//     the server. The registration answer names the WHIP and MQTT addresses
//     the device uses next; the server derives them from its own network
//     (unreachable from the LAN), so they are rewritten to point back here.
//   - MQTT (device control) is forwarded as plain TCP.
//   - Media is not relayed: WebRTC is UDP, and the device and the server
//     meet directly through STUN.
//
//	view-relay -listen 192.168.1.139 -http 28070 -mqtt 21883 \
//	    -upstream 100.96.30.88:8070 -upstream-mqtt 100.96.30.88:1883
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strconv"
	"strings"
	"time"
)

func main() {
	listen := flag.String("listen", "", "LAN address devices reach this machine at (required)")
	httpPort := flag.Int("http", 28070, "HTTP port devices use as their endpoint")
	mqttPort := flag.Int("mqtt", 21883, "MQTT port handed to devices")
	upstream := flag.String("upstream", "", "server HTTP address, host:port (required)")
	upstreamMQTT := flag.String("upstream-mqtt", "", "server MQTT address, host:port (required)")
	flag.Parse()
	if *listen == "" || *upstream == "" || *upstreamMQTT == "" {
		flag.Usage()
		log.Fatal("view-relay: -listen, -upstream and -upstream-mqtt are required")
	}
	httpAddr := net.JoinHostPort(*listen, strconv.Itoa(*httpPort))
	mqttAddr := net.JoinHostPort(*listen, strconv.Itoa(*mqttPort))

	go forwardTCP(mqttAddr, *upstreamMQTT)

	target := &url.URL{Scheme: "http", Host: *upstream}
	proxy := httputil.NewSingleHostReverseProxy(target)
	proxy.ModifyResponse = func(resp *http.Response) error {
		if !isRegistration(resp.Request) || resp.StatusCode != http.StatusOK {
			return nil
		}
		return rewriteRegistration(resp, "http://"+httpAddr, "tcp://"+mqttAddr)
	}
	log.Printf("view-relay: HTTP %s -> %s, MQTT %s -> %s", httpAddr, *upstream, mqttAddr, *upstreamMQTT)
	server := &http.Server{Addr: httpAddr, Handler: proxy, ReadHeaderTimeout: 10 * time.Second}
	log.Fatal(server.ListenAndServe())
}

// isRegistration matches the gateway's registration routes ("POST /" is the
// legacy alias of /api/device/v1/bootstrap).
func isRegistration(r *http.Request) bool {
	return r != nil && r.Method == http.MethodPost && (r.URL.Path == "/api/device/v1/bootstrap" || r.URL.Path == "/")
}

// rewriteRegistration points whipUrl and mqttBroker of a registration answer
// at this relay, keeping the WHIP path.
func rewriteRegistration(resp *http.Response, httpBase, mqttURL string) error {
	body, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		return err
	}
	var doc map[string]any
	if err := json.Unmarshal(body, &doc); err != nil {
		// Not the JSON we expect: pass it on untouched.
		resp.Body = io.NopCloser(bytes.NewReader(body))
		return nil
	}
	if whip, ok := doc["whipUrl"].(string); ok {
		if u, err := url.Parse(whip); err == nil {
			doc["whipUrl"] = strings.TrimRight(httpBase, "/") + u.EscapedPath()
		}
	}
	if _, ok := doc["mqttBroker"]; ok {
		doc["mqttBroker"] = mqttURL
	}
	out, err := json.Marshal(doc)
	if err != nil {
		return err
	}
	resp.Body = io.NopCloser(bytes.NewReader(out))
	resp.ContentLength = int64(len(out))
	resp.Header.Set("Content-Length", strconv.Itoa(len(out)))
	log.Printf("view-relay: registration of %s rewritten: whip %v, mqtt %v", resp.Request.RemoteAddr, doc["whipUrl"], doc["mqttBroker"])
	return nil
}

// forwardTCP copies every connection on addr to upstream, both ways.
func forwardTCP(addr, upstream string) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("view-relay: MQTT listen %s: %v", addr, err)
	}
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("view-relay: MQTT accept: %v", err)
			time.Sleep(time.Second)
			continue
		}
		go func() {
			defer conn.Close()
			up, err := net.DialTimeout("tcp", upstream, 10*time.Second)
			if err != nil {
				log.Printf("view-relay: MQTT %s -> %s: %v", conn.RemoteAddr(), upstream, err)
				return
			}
			defer up.Close()
			done := make(chan struct{}, 2)
			go func() { _, _ = io.Copy(up, conn); closeWrite(up); done <- struct{}{} }()
			go func() { _, _ = io.Copy(conn, up); closeWrite(conn); done <- struct{}{} }()
			<-done
			<-done
		}()
	}
}

func closeWrite(c net.Conn) {
	if tc, ok := c.(*net.TCPConn); ok {
		_ = tc.CloseWrite()
	}
}
