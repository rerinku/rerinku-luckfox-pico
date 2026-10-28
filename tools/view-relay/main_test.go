package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"testing"
)

func TestRewriteRegistration(t *testing.T) {
	answer := `{"device":{"id":1},"whipUrl":"http://172.28.1.2:8070/whip/LUCKFOX-1","mqttBroker":"tcp://172.28.1.2:1883","deviceToken":"t"}`
	resp := &http.Response{StatusCode: 200, Header: http.Header{}, Body: io.NopCloser(bytes.NewBufferString(answer)),
		Request: &http.Request{Method: "POST", URL: &url.URL{Path: "/api/device/v1/bootstrap"}}}
	if !isRegistration(resp.Request) {
		t.Fatal("registration not recognised")
	}
	if err := rewriteRegistration(resp, "http://192.168.1.139:28070", "tcp://192.168.1.139:21883"); err != nil {
		t.Fatal(err)
	}
	var doc map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&doc); err != nil {
		t.Fatal(err)
	}
	if doc["whipUrl"] != "http://192.168.1.139:28070/whip/LUCKFOX-1" || doc["mqttBroker"] != "tcp://192.168.1.139:21883" || doc["deviceToken"] != "t" {
		t.Fatalf("rewritten = %v", doc)
	}
	if isRegistration(&http.Request{Method: "POST", URL: &url.URL{Path: "/whip/X"}}) {
		t.Fatal("WHIP is not registration")
	}
}
