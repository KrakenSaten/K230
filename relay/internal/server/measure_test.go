// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

//go:build linux

package server

// Measurements M1-M4 of RESOLUTION-review-1.md §12. They run only with
// RELAY_MEASURE=1, print their figures, and assert nothing but that they
// ran: docs/api/relay.md §15 records the figures with the machine.

import (
	"bufio"
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

func measureOn(t *testing.T) {
	if os.Getenv("RELAY_MEASURE") == "" {
		t.Skip("set RELAY_MEASURE=1 to run the measurements")
	}
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func machine(t *testing.T) {
	model := ""
	if b, err := os.ReadFile("/proc/cpuinfo"); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if strings.HasPrefix(l, "model name") {
				model = strings.TrimSpace(strings.SplitN(l, ":", 2)[1])
				break
			}
		}
	}
	mem := ""
	if b, err := os.ReadFile("/proc/meminfo"); err == nil {
		mem = strings.TrimSpace(strings.SplitN(string(b), "\n", 2)[0])
	}
	kern, _ := os.ReadFile("/proc/sys/kernel/osrelease")
	t.Logf("machine: %s, %d CPUs, %s, kernel %s, %s %s/%s", model, runtime.NumCPU(), mem,
		strings.TrimSpace(string(kern)), runtime.Version(), runtime.GOOS, runtime.GOARCH)
}

func rssKB() int64 {
	b, _ := os.ReadFile("/proc/self/status")
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "VmRSS:") {
			f := strings.Fields(l)
			n, _ := strconv.ParseInt(f[1], 10, 64)
			return n
		}
	}
	return 0
}

func cpuTime() time.Duration {
	var ru syscall.Rusage
	syscall.Getrusage(syscall.RUSAGE_SELF, &ru)
	return time.Duration(ru.Utime.Nano() + ru.Stime.Nano())
}

func heapMB() float64 {
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	return float64(m.HeapInuse) / (1 << 20)
}

func pct(ds []time.Duration, p float64) time.Duration {
	if len(ds) == 0 {
		return 0
	}
	s := append([]time.Duration(nil), ds...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	i := int(p * float64(len(s)-1))
	return s[i]
}

func from(ip string, to *target) fake.Dialer {
	return func(ctx context.Context) (net.Conn, error) {
		d := net.Dialer{LocalAddr: &net.TCPAddr{IP: net.ParseIP(ip)}, Timeout: 5 * time.Second}
		return d.DialContext(ctx, "tcp", to.v.Load().(string))
	}
}

func realLimits() Limits {
	return Limits{PerSourceRate: DefaultPerSourceRate, PerSourceUnauth: DefaultPerSourceUnauth,
		MaxConns: DefaultMaxConns, MaxSources: DefaultMaxSources, SessionIdleSeconds: DefaultSessionIdle}
}

// M1: connection churn from many sources, with a legitimate unit and phone
// reconnecting through it.
func TestM1Churn(t *testing.T) {
	measureOn(t)
	machine(t)
	secs := envInt("RELAY_M1_SECONDS", 60)
	rate := envInt("RELAY_M1_RATE", 2000)
	tm := defaultTiming(realLimits())
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := fake.NewUnit(relayName, 443, from("127.0.0.1", to))
	v := fake.NewUnit(relayName, 443, from("127.0.0.3", to))
	t.Cleanup(u.Disconnect)
	e := start(t, ln, opts{units: [][32]byte{u.ID, v.ID}, limits: realLimits(), timing: &tm, logOut: io.Discard})
	e.setUp(u)
	v.Set(func(x *fake.Unit) { a := e.s.KeyHash(); x.A, x.ConfigID, x.Seq = &a, 1, 1 })
	p := fake.NewPhone(from("127.0.0.2", to))
	rec := pairLocally(t, u, p, true)

	runtime.GC()
	rss0, heap0, cpu0, t0 := rssKB(), heapMB(), cpuTime(), time.Now()
	child := exec.Command(os.Args[0], "-test.run=^TestM1ChurnChild$", "-test.v")
	child.Env = append(os.Environ(), "RELAY_CHURN_ADDR="+e.addr,
		"RELAY_CHURN_SECONDS="+strconv.Itoa(secs), "RELAY_CHURN_RATE="+strconv.Itoa(rate))
	out, _ := child.StdoutPipe()
	if err := child.Start(); err != nil {
		t.Fatal(err)
	}

	var peakRSS int64
	var peakHeap float64
	var peakSrc, peakLive, peakG int
	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		tk := time.NewTicker(time.Second)
		defer tk.Stop()
		for {
			select {
			case <-stop:
				return
			case <-tk.C:
				peakRSS = max(peakRSS, rssKB())
				peakHeap = max(peakHeap, heapMB())
				peakSrc = max(peakSrc, e.s.lim.size())
				e.s.lim.mu.Lock()
				peakLive = max(peakLive, e.s.lim.live)
				e.s.lim.mu.Unlock()
				peakG = max(peakG, runtime.NumGoroutine())
			}
		}
	}()
	var ctlLat, phoneLat []time.Duration
	var ctlFail, phoneFail int
	deadline := time.Now().Add(time.Duration(secs) * time.Second)
	vt := newTarget(e.addr)
	for time.Now().Before(deadline) {
		a := time.Now()
		ok := func() bool {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			c, err := fake.DialRelay(ctx, from("127.0.0.3", vt), relayName, nil)
			if err != nil {
				return false
			}
			defer c.Close()
			if c.Start(relayName, 443) != nil {
				return false
			}
			sig, _ := wire.Sign(v.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
			c.Send(&wire.UnitLogin{SPKI: v.SPKI, Sig: sig})
			c.Send(&wire.Register{Endorsement: v.Endorse()})
			for {
				m, err := c.Recv()
				if err != nil {
					return false
				}
				if m.Type() == "registered" {
					return true
				}
			}
		}()
		if ok {
			ctlLat = append(ctlLat, time.Since(a))
		} else {
			ctlFail++
		}
		a = time.Now()
		if s, err := p.Connect(ctx(t), rec, nil); err == nil {
			phoneLat = append(phoneLat, time.Since(a))
			s.Close()
		} else {
			phoneFail++
		}
		time.Sleep(250 * time.Millisecond)
	}
	summary, _ := io.ReadAll(out)
	child.Wait()
	close(stop)
	wg.Wait()
	wall, cpu := time.Since(t0), cpuTime()-cpu0
	c := &e.s.count
	t.Logf("M1: %d s churn at a target of %d/s from random 127/8 sources; child: %s", secs, rate,
		strings.TrimSpace(lastLine(string(summary), "CHURN")))
	t.Logf("M1: relay process RSS %.1f -> peak %.1f MB, Go heap in use %.1f -> peak %.1f MB, goroutines peak %d",
		float64(rss0)/1024, float64(peakRSS)/1024, heap0, peakHeap, peakG)
	t.Logf("M1: CPU %.1f s over %.1f s wall (%.2f cores), source table peak %d of %d, live connections peak %d of %d",
		cpu.Seconds(), wall.Seconds(), cpu.Seconds()/wall.Seconds(), peakSrc, DefaultMaxSources, peakLive, DefaultMaxConns)
	t.Logf("M1: refused at accept: sources %d, rate %d, unauth %d, global %d; tls failed %d; ended before login %d",
		c.refused[refusedSources].Load(), c.refused[refusedRate].Load(), c.refused[refusedUnauth].Load(),
		c.refused[refusedGlobal].Load(), c.tls.Load(), c.early.Load())
	t.Logf("M1: unit control dial to registered: n=%d p50 %v p99 %v max %v, failed %d",
		len(ctlLat), pct(ctlLat, .5), pct(ctlLat, .99), pct(ctlLat, 1), ctlFail)
	t.Logf("M1: phone dial to session (relay ready + inner TLS): n=%d p50 %v p99 %v max %v, failed %d",
		len(phoneLat), pct(phoneLat, .5), pct(phoneLat, .99), pct(phoneLat, 1), phoneFail)
}

func lastLine(s, prefix string) string {
	l := ""
	for _, x := range strings.Split(s, "\n") {
		if strings.Contains(x, prefix) {
			l = x
		}
	}
	return l
}

// TestM1ChurnChild is M1's load, run as a separate process.
func TestM1ChurnChild(t *testing.T) {
	addr := os.Getenv("RELAY_CHURN_ADDR")
	if addr == "" {
		t.Skip("M1's child process only")
	}
	secs, _ := strconv.Atoi(os.Getenv("RELAY_CHURN_SECONDS"))
	rate, _ := strconv.Atoi(os.Getenv("RELAY_CHURN_RATE"))
	const workers = 64
	var attempts, tlsN, errs atomic.Int64
	deadline := time.Now().Add(time.Duration(secs) * time.Second)
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			r := rand.New(rand.NewSource(int64(w)))
			tick := time.NewTicker(time.Second * workers / time.Duration(rate))
			defer tick.Stop()
			for time.Now().Before(deadline) {
				<-tick.C
				ip := net.IPv4(127, byte(1+r.Intn(250)), byte(r.Intn(256)), byte(1+r.Intn(254)))
				d := net.Dialer{LocalAddr: &net.TCPAddr{IP: ip}, Timeout: time.Second}
				c, err := d.Dial("tcp", addr)
				attempts.Add(1)
				if err != nil {
					errs.Add(1)
					continue
				}
				if r.Intn(10) < 3 {
					tc := tls.Client(c, &tls.Config{ServerName: relayName, InsecureSkipVerify: true, NextProtos: []string{"http/1.1"}})
					tc.SetDeadline(time.Now().Add(2 * time.Second))
					if tc.Handshake() == nil {
						tlsN.Add(1)
					}
				}
				c.Close()
			}
		}(w)
	}
	wg.Wait()
	fmt.Printf("CHURN attempts=%d (%.0f/s) tls_handshakes=%d dial_errors=%d\n",
		attempts.Load(), float64(attempts.Load())/float64(secs), tlsN.Load(), errs.Load())
}

// retryConnect is the app's back-off: 1 s doubling to 60 s, with jitter.
func retryConnect(p *fake.Phone, rec *fake.Record, limit time.Duration) (time.Duration, int, error) {
	t0 := time.Now()
	wait := time.Second
	for tries := 1; ; tries++ {
		c, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		s, err := p.Connect(c, rec, nil)
		cancel()
		if err == nil {
			s.Close()
			return time.Since(t0), tries, nil
		}
		if time.Since(t0) > limit {
			return time.Since(t0), tries, err
		}
		time.Sleep(time.Duration(float64(wait) * (0.8 + 0.4*rand.Float64())))
		wait = min(2*wait, 60*time.Second)
	}
}

// M2: many legitimate phones behind one source, at the default limits.
func TestM2SharedNAT(t *testing.T) {
	measureOn(t)
	machine(t)
	n := envInt("RELAY_M2_PHONES", 40)
	tm := defaultTiming(realLimits())
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	var units []*fake.Unit
	var ids [][32]byte
	for i := 0; i < n; i++ {
		u := fake.NewUnit(relayName, 443, from("127.0.3."+strconv.Itoa(i+1), to))
		units = append(units, u)
		ids = append(ids, u.ID)
		t.Cleanup(u.Disconnect)
	}
	e := start(t, ln, opts{units: ids, limits: realLimits(), timing: &tm, logOut: io.Discard})
	recs := make([]*fake.Record, n)
	phones := make([]*fake.Phone, n)
	for i, u := range units {
		e.setUp(u)
		phones[i] = fake.NewPhone(from("127.0.4.1", to))
		recs[i] = pairLocally(t, u, phones[i], true)
	}
	time.Sleep(1100 * time.Millisecond) // the shared source's bucket is full again
	lat := make([]time.Duration, n)
	tries := make([]int, n)
	var fails atomic.Int64
	var wg sync.WaitGroup
	start := make(chan struct{})
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			d, k, err := retryConnect(phones[i], recs[i], 2*time.Minute)
			lat[i], tries[i] = d, k
			if err != nil {
				fails.Add(1)
			}
		}(i)
	}
	close(start)
	wg.Wait()
	first, refusedTries := 0, 0
	for _, k := range tries {
		if k == 1 {
			first++
		}
		refusedTries += k - 1
	}
	c := &e.s.count
	t.Logf("M2: %d phones behind one source (per source: %d new/s, %d unauthenticated), all at once, app back-off 1 s doubling",
		n, DefaultPerSourceRate, DefaultPerSourceUnauth)
	t.Logf("M2: %d in on the first try, %d failed attempts in all, %d never in; time to session p50 %v p90 %v max %v",
		first, refusedTries, fails.Load(), pct(lat, .5), pct(lat, .9), pct(lat, 1))
	t.Logf("M2: refused at accept: rate %d, unauth %d, sources %d, global %d",
		c.refused[refusedRate].Load(), c.refused[refusedUnauth].Load(), c.refused[refusedSources].Load(), c.refused[refusedGlobal].Load())
}

// M3: a splice whose phone stops reading, and a control connection that
// stops reading.
func TestM3SlowReader(t *testing.T) {
	measureOn(t)
	machine(t)
	tm := slowTiming()
	tm.sessionIdle = 90 * time.Second // the default: no splice here may end by idleness
	e, to, us := rawRelay(t, opts{timing: tm}, 4)
	u1, u2, u3 := us[0], us[1], us[2]
	p1, p2, p3 := newPhone(to), newPhone(to), newPhone(to)
	k1 := ctlRegister(t, to, u1, p1.FP)
	k2 := ctlRegister(t, to, u2, p2.FP)
	pc2, dc2 := runningSplice(t, to, k2, p2, u2, nil)

	throughput := func() float64 {
		const total = 64 << 20
		done := make(chan time.Duration)
		go func() {
			t0 := time.Now()
			pc2.TLS.SetReadDeadline(time.Now().Add(60 * time.Second))
			if _, err := io.CopyN(io.Discard, pc2.BR, total); err != nil {
				t.Error("throughput read:", err)
			}
			done <- time.Since(t0)
		}()
		buf := make([]byte, 64<<10)
		dc2.TLS.SetWriteDeadline(time.Now().Add(60 * time.Second))
		for sent := 0; sent < total; sent += len(buf) {
			if _, err := dc2.TLS.Write(buf); err != nil {
				t.Fatal("throughput write:", err)
			}
		}
		return float64(total) / (1 << 20) / (<-done).Seconds()
	}
	rtt := func() (time.Duration, time.Duration) {
		var ds []time.Duration
		b := make([]byte, 1)
		pc2.TLS.SetDeadline(time.Now().Add(30 * time.Second))
		dc2.TLS.SetDeadline(time.Now().Add(30 * time.Second))
		for i := 0; i < 200; i++ {
			t0 := time.Now()
			pc2.TLS.Write([]byte{'x'})
			if _, err := io.ReadFull(dc2.BR, b); err != nil {
				t.Fatal("rtt:", err)
			}
			dc2.TLS.Write([]byte{'y'})
			if _, err := io.ReadFull(pc2.BR, b); err != nil {
				t.Fatal("rtt:", err)
			}
			ds = append(ds, time.Since(t0))
		}
		return pct(ds, .5), pct(ds, .99)
	}
	runtime.GC()
	tp0 := throughput()
	r50, r99 := rtt()
	heap0, rss0 := heapMB(), rssKB()

	_, dc1 := runningSplice(t, to, k1, p1, u1, nil) // its phone never reads
	stuck := make(chan int64)
	go func() {
		var sent int64
		buf := make([]byte, 64<<10)
		for {
			dc1.TLS.SetWriteDeadline(time.Now().Add(time.Second))
			n, err := dc1.TLS.Write(buf)
			sent += int64(n)
			if err != nil {
				stuck <- sent
				return
			}
		}
	}()
	buffered := <-stuck
	time.Sleep(5 * time.Second)
	runtime.GC()
	heap1, rss1 := heapMB(), rssKB()
	tp1 := throughput()
	r50b, r99b := rtt()
	t.Logf("M3a: a splice whose phone stopped reading took %.1f MB before its unit's writes blocked (socket buffers on both legs)",
		float64(buffered)/(1<<20))
	t.Logf("M3a: relay Go heap %.1f -> %.1f MB, RSS %.1f -> %.1f MB, 5 s after it blocked",
		heap0, heap1, float64(rss0)/1024, float64(rss1)/1024)
	t.Logf("M3a: another splice: throughput %.0f MB/s alone, %.0f MB/s beside it; echo RTT p50/p99 %v/%v alone, %v/%v beside it",
		tp0, tp1, r50, r99, r50b, r99b)

	// M3b: a control connection that does not read (receive buffer 4 KiB),
	// sent invitations by a listed phone reconnecting: paced at about
	// 200/s, and in a burst of 32 at once.
	flood := func(u *fake.Unit, p *fake.Phone, workers int, pause time.Duration, net3 int) {
		small := func(ctx context.Context) (net.Conn, error) {
			c, err := fake.DialTCP(e.addr)(ctx)
			if err == nil {
				c.(*net.TCPConn).SetReadBuffer(4096)
			}
			return c, err
		}
		kc, err := fake.DialRelay(ctx(t), small, relayName, nil)
		if err != nil {
			t.Fatal(err)
		}
		defer kc.Close()
		kc.Start(relayName, 443)
		sig, _ := wire.Sign(u.Key, wire.UnitLoginMsg(kc.Nonce, kc.Presented))
		kc.Send(&wire.UnitLogin{SPKI: u.SPKI, Sig: sig})
		kc.Send(&wire.Register{Admit: [][32]byte{p.FP}, Endorsement: u.Endorse()})
		waitFor(t, "online", func() bool { return e.online(u.ID) })
		var invites atomic.Int64
		t0 := time.Now()
		stopFlood := make(chan struct{})
		var wg sync.WaitGroup
		for w := 0; w < workers; w++ {
			wg.Add(1)
			go func(w int) {
				defer wg.Done()
				for i := 0; ; i++ {
					select {
					case <-stopFlood:
						return
					case <-time.After(pause):
					}
					ip := "127.0." + strconv.Itoa(net3) + "." + strconv.Itoa(1+(w*7+i)%250)
					c, err := fake.DialRelay(context.Background(), from(ip, newTarget(e.addr)), relayName, nil)
					if err != nil {
						continue
					}
					if c.Start(relayName, 443) == nil {
						sig, _ := wire.Sign(p.Key, wire.ClientLoginMsg(c.Nonce, c.Presented, u.ID))
						c.Send(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: u.ID, Sig: sig})
						c.Send(&wire.Connect{})
						invites.Add(1)
					}
					c.Close()
				}
			}(w)
		}
		for e.online(u.ID) && time.Since(t0) < 60*time.Second {
			time.Sleep(5 * time.Millisecond)
		}
		gone := time.Since(t0)
		close(stopFlood)
		wg.Wait()
		reason := ""
		waitFor(t, "control down logged", func() bool {
			for _, l := range strings.Split(e.logs.String(), "\n") {
				if strings.Contains(l, "control down") && strings.Contains(l, "unit="+wire.Prefix(u.ID)) {
					reason = l[strings.Index(l, "reason="):]
					return true
				}
			}
			return false
		})
		t.Logf("M3b: %d phone connects at a time, a pause of %v each: control disconnected after %v and about %d invitations: %s",
			workers, pause, gone.Round(time.Millisecond), invites.Load(), reason)
	}
	flood(u3, p3, 1, 5*time.Millisecond, 6)
	flood(us[3], newPhone(to), 32, 0, 7)
}

// delayProxy forwards TCP to addr, delaying each direction by d.
func delayProxy(t *testing.T, addr string, d time.Duration) string {
	ln := listen(t)
	t.Cleanup(func() { ln.Close() })
	pipe := func(dst, src net.Conn) {
		type chunk struct {
			at time.Time
			b  []byte
		}
		ch := make(chan chunk, 4096)
		go func() {
			defer close(ch)
			r := bufio.NewReader(src)
			for {
				b := make([]byte, 32<<10)
				n, err := r.Read(b)
				if n > 0 {
					ch <- chunk{time.Now(), b[:n]}
				}
				if err != nil {
					return
				}
			}
		}()
		for c := range ch {
			time.Sleep(time.Until(c.at.Add(d)))
			if _, err := dst.Write(c.b); err != nil {
				break
			}
		}
		dst.Close()
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			up, err := net.Dial("tcp", addr)
			if err != nil {
				c.Close()
				continue
			}
			go pipe(up, c)
			go pipe(c, up)
		}
	}()
	return ln.Addr().String()
}

// M4: the provisional path over a slow link, against the 10 s deadline.
func TestM4SlowLink(t *testing.T) {
	measureOn(t)
	machine(t)
	got := make(chan time.Duration, 64)
	tm := defaultTiming(realLimits())
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	e := start(t, ln, opts{units: [][32]byte{u.ID}, timing: &tm, logOut: io.Discard,
		hook: func(p string, v any) {
			if p == "connect" {
				got <- v.(time.Duration)
			}
		}})
	e.setUp(u)
	for _, rtt := range []time.Duration{300 * time.Millisecond, time.Second, 2500 * time.Millisecond} {
		px := delayProxy(t, e.addr, rtt/2)
		p := fake.NewPhone(fake.DialTCP(px))
		base := pairLocally(t, u, p, true)
		var ds []time.Duration
		ok := 0
		for i := 0; i < 5; i++ {
			rec := *base
			rec.Pin = nil // the provisional path: which, endorsement, then client
			s, err := p.Connect(ctx(t), &rec, nil)
			select {
			case d := <-got:
				ds = append(ds, d)
			case <-time.After(30 * time.Second):
			}
			if err == nil {
				ok++
				s.Close()
			}
		}
		t.Logf("M4: RTT %v: relay accept to connect p50 %v max %v (deadline 10 s); %d of 5 sessions up",
			rtt, pct(ds, .5).Round(time.Millisecond), pct(ds, 1).Round(time.Millisecond), ok)
	}
}
