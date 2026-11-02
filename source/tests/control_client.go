// SPDX-License-Identifier: GPL-3.0-or-later
// Independent standard-library wire client for the native control upgrade.
package main

import (
	"bufio"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha512"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/textproto"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type row struct {
	Mode     int    `json:"mode"`
	M3       string `json:"m3"`
	Envelope string `json:"envelope"`
	Material string `json:"material"`
}

func check(ok bool, msg string) {
	if !ok {
		panic(msg)
	}
}
func unhex(s string) []byte {
	b, e := hex.DecodeString(s)
	if e != nil {
		panic(e)
	}
	return b
}
func must(e error) {
	if e != nil {
		panic(e)
	}
}

type message struct {
	line    string
	headers textproto.MIMEHeader
	body    []byte
}

func read(r *bufio.Reader) message {
	line, e := r.ReadString('\n')
	must(e)
	h, e := textproto.NewReader(r).ReadMIMEHeader()
	must(e)
	n, _ := strconv.Atoi(h.Get("Content-Length"))
	check(n >= 0 && n < 1000000, "response bound")
	b := make([]byte, n)
	_, e = io.ReadFull(r, b)
	must(e)
	return message{line, h, b}
}
func status(m message, n int) {
	check(strings.Contains(m.line, fmt.Sprintf(" %d ", n)), fmt.Sprintf("status %d: %s", n, m.line))
}
func request(w io.Writer, r *bufio.Reader, method, path, protocol string, body []byte, pd bool) message {
	headers := ""
	if protocol == "RTSP/1.0" {
		headers = "CSeq: 1\r\n"
	}
	if pd {
		headers += "X-Apple-PD: 1\r\n"
	}
	_, e := fmt.Fprintf(w, "%s %s %s\r\nHost: localhost\r\n%sContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n", method, path, protocol, headers, len(body))
	must(e)
	_, e = w.Write(body)
	must(e)
	return read(r)
}
func pair(c net.Conn, r *bufio.Reader) []byte {
	pub, priv, e := ed25519.GenerateKey(rand.Reader)
	must(e)
	curve, e := ecdh.X25519().GenerateKey(rand.Reader)
	must(e)
	m := request(c, r, "POST", "/pair-setup", "RTSP/1.0", pub, true)
	status(m, 200)
	check(len(m.body) == 32, "receiver identity")
	serverIdentity := m.body
	payload := append([]byte{1, 0, 0, 0}, curve.PublicKey().Bytes()...)
	payload = append(payload, pub...)
	m = request(c, r, "POST", "/pair-verify", "RTSP/1.0", payload, true)
	status(m, 200)
	check(len(m.body) == 96, "verify response")
	server, e := ecdh.X25519().NewPublicKey(m.body[:32])
	must(e)
	secret, e := curve.ECDH(server)
	must(e)
	key := sha512.Sum512(append([]byte("Pair-Verify-AES-Key"), secret...))
	iv := sha512.Sum512(append([]byte("Pair-Verify-AES-IV"), secret...))
	block, e := aes.NewCipher(key[:16])
	must(e)
	ctr := cipher.NewCTR(block, iv[:16])
	signature := make([]byte, 64)
	ctr.XORKeyStream(signature, m.body[32:])
	check(ed25519.Verify(serverIdentity, append(append([]byte{}, server.Bytes()...), curve.PublicKey().Bytes()...), signature), "receiver signature")
	signature = ed25519.Sign(priv, append(append([]byte{}, curve.PublicKey().Bytes()...), server.Bytes()...))
	ctr.XORKeyStream(signature, signature)
	m = request(c, r, "POST", "/pair-verify", "RTSP/1.0", append([]byte{0, 0, 0, 0}, signature...), true)
	status(m, 200)
	return secret
}

type direction struct {
	aead  cipher.AEAD
	nonce []byte
}

func state(key, nonce, secret []byte) direction {
	if secret != nil {
		h := sha512.Sum512(append(append([]byte{}, key...), secret...))
		key = h[:16]
	}
	b, e := aes.NewCipher(key)
	must(e)
	g, e := cipher.NewGCMWithNonceSize(b, 16)
	must(e)
	return direction{g, append([]byte{}, nonce...)}
}
func (d *direction) next() {
	for i := 15; i >= 0; i-- {
		d.nonce[i]++
		if d.nonce[i] != 0 {
			return
		}
	}
	panic("nonce exhausted")
}

type records struct {
	net.Conn
	read, write direction
	remaining   []byte
	fragment    bool
}

func (c *records) Write(p []byte) (int, error) {
	c.write.next()
	h := make([]byte, 4)
	binary.BigEndian.PutUint32(h, uint32(len(p)))
	b := c.write.aead.Seal(h, c.write.nonce, p, h)
	if c.fragment {
		for _, v := range b {
			_, e := c.Conn.Write([]byte{v})
			if e != nil {
				return 0, e
			}
		}
		c.fragment = false
		return len(p), nil
	}
	for len(b) > 0 {
		n, e := c.Conn.Write(b)
		if e != nil {
			return 0, e
		}
		b = b[n:]
	}
	return len(p), nil
}
func (c *records) Read(p []byte) (int, error) {
	for len(c.remaining) == 0 {
		var h [4]byte
		if _, e := io.ReadFull(c.Conn, h[:]); e != nil {
			return 0, e
		}
		n := binary.BigEndian.Uint32(h[:])
		check(n <= 0x40000, "encrypted length bound")
		b := make([]byte, int(n)+16)
		if _, e := io.ReadFull(c.Conn, b); e != nil {
			return 0, e
		}
		c.read.next()
		plain, e := c.read.aead.Open(nil, c.read.nonce, b, h[:])
		if e != nil {
			return 0, e
		}
		c.remaining = plain
	}
	n := copy(p, c.remaining)
	c.remaining = c.remaining[n:]
	return n, nil
}
func setup(address string, v row, mix bool) (*records, *bufio.Reader) {
	c, e := net.DialTimeout("tcp", address, 2*time.Second)
	must(e)
	must(c.SetDeadline(time.Now().Add(10 * time.Second)))
	r := bufio.NewReader(c)
	var secret []byte
	if mix {
		secret = pair(c, r)
	}
	status(request(c, r, "GET", "/server-info", "HTTP/1.1", nil, false), 200)
	initial := unhex("46504c590301010000000004020000bb")
	initial[14] = byte(v.Mode)
	m := request(c, r, "POST", "/fp-setup", "HTTP/1.1", initial, false)
	status(m, 200)
	check(len(m.body) == 142, "first setup response")
	m = request(c, r, "POST", "/fp-setup", "HTTP/1.1", unhex(v.M3), false)
	status(m, 200)
	check(len(m.body) == 32, "second setup response")
	m = request(c, r, "POST", "/fp-setup2", "HTTP/1.1", unhex(v.Envelope), mix)
	status(m, 200)
	check(len(m.body) == 0, "plaintext upgrade acknowledgement")
	material := unhex(v.Material)
	encrypted := &records{Conn: c, write: state(material[:16], material[16:32], secret), read: state(material[32:48], material[48:], secret), fragment: true}
	return encrypted, bufio.NewReader(encrypted)
}
func main() {
	check(len(os.Args) == 3 || (len(os.Args) == 4 && os.Args[3] == "--probe"), "usage: control_client address fixture.json [--probe]")
	var fixtures struct {
		Sessions []row
		HLSPlay  string `json:"hls_play"`
	}
	b, e := os.ReadFile(os.Args[2])
	must(e)
	must(json.Unmarshal(b, &fixtures))
	if len(os.Args) == 4 {
		for _, mix := range []bool{false, true} {
			for _, v := range fixtures.Sessions {
				c, r := setup(os.Args[1], v, mix)
				status(request(c, r, "GET", "/playback-info", "HTTP/1.1", nil, false), 200)
				c.Close()
				time.Sleep(20 * time.Millisecond)
			}
		}
		c, r := setup(os.Args[1], fixtures.Sessions[0], true)
		status(request(c, r, "POST", "/play", "HTTP/1.1", []byte("Content-Location: http://127.0.0.1:9080/test-video.mp4\r\nStart-Position: 0\r\n"), false), 200)
		time.Sleep(4 * time.Second)
		status(request(c, r, "GET", "/playback-info", "HTTP/1.1", nil, false), 200)
		status(request(c, r, "POST", "/stop", "HTTP/1.1", nil, false), 200)
		c.Close()
		fmt.Println("Hardware probe: four modes, paired/unpaired encrypted playback-info, paired encrypted local test playback/stop completed")
		return
	}
	for _, mix := range []bool{false, true} {
		for _, v := range fixtures.Sessions {
			c, r := setup(os.Args[1], v, mix)
			m := request(c, r, "GET", "/playback-info", "HTTP/1.1", nil, false)
			status(m, 200)
			check(bytes.Contains(m.body, []byte("readyToPlay")), "decrypted playback info")
			m = request(c, r, "POST", "/play", "HTTP/1.1", []byte("Content-Location: https://test.invalid/encrypted.mp4\r\nStart-Position: 0\r\n"), false)
			status(m, 200)
			status(request(c, r, "POST", "/rate?value=0", "HTTP/1.1", nil, false), 200)
			status(request(c, r, "POST", "/stop", "HTTP/1.1", nil, false), 200)
			c.Close()
			time.Sleep(20 * time.Millisecond)
		}
	}
	// Fail closed on corrupted, replayed, and oversized records; no plaintext
	// protocol error must escape onto a connection after the upgrade.
	for _, kind := range []string{"tamper", "replay", "oversized"} {
		c, r := setup(os.Args[1], fixtures.Sessions[0], false)
		plain := []byte("GET /playback-info HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n")
		c.write.next()
		header := make([]byte, 4)
		binary.BigEndian.PutUint32(header, uint32(len(plain)))
		frame := c.write.aead.Seal(header, c.write.nonce, plain, header)
		if kind == "replay" {
			_, e = c.Conn.Write(frame)
			must(e)
			status(read(r), 200)
		}
		if kind == "tamper" {
			frame[len(frame)-1] ^= 1
		}
		if kind == "oversized" {
			frame = make([]byte, 4)
			binary.BigEndian.PutUint32(frame, 0x40001)
		}
		_, e = c.Conn.Write(frame)
		must(e)
		var probe [1]byte
		n, err := c.Conn.Read(probe[:])
		check(n == 0 && (errors.Is(err, io.EOF) || errors.Is(err, syscall.ECONNRESET)), "corrupt record must close immediately: "+kind)
		c.Close()
	}
	// An encrypted reverse socket must also encrypt unsolicited FCUP messages.
	c, r := setup(os.Args[1], fixtures.Sessions[0], false)
	reverse, rr := setup(os.Args[1], fixtures.Sessions[1], false)
	_, e = fmt.Fprint(reverse, "POST /reverse HTTP/1.1\r\nHost: localhost\r\nX-Apple-Session-ID: 77777777-7777-4777-8777-777777777777\r\nConnection: Upgrade\r\nUpgrade: PTTH/1.0\r\nX-Apple-Purpose: event\r\nContent-Length: 0\r\n\r\n")
	must(e)
	status(read(rr), 101)
	payload := unhex(fixtures.HLSPlay)
	_, e = fmt.Fprintf(c, "POST /play HTTP/1.1\r\nHost: localhost\r\nX-Apple-Session-ID: 77777777-7777-4777-8777-777777777777\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: %d\r\n\r\n", len(payload))
	must(e)
	_, e = c.Write(payload)
	must(e)
	status(read(r), 200)
	event := read(rr)
	check(strings.HasPrefix(event.line, "POST /event ") && bytes.Contains(event.body, []byte("FCUP_Response_URL")), "encrypted unsolicited FCUP event")
	_, e = fmt.Fprint(reverse, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
	must(e)
	status(request(c, r, "POST", "/stop", "HTTP/1.1", nil, false), 200)
	reverse.Close()
	c.Close()
	fmt.Println("Independent Go client: all four SAP modes, encrypted play/info/rate/stop, fragmented records, verified pairing and key mixing, encrypted reverse FCUP, tamper/replay/oversize rejection passed")
}
