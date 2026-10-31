#!/usr/bin/env python3
"""
Vehicle Gateway dashboard  (laptop side, Python 3 standard library only, no installs)

It connects to the Gateway's JSON stats feed (Pi, TCP port 6100), keeps the last
few minutes of history and serves a live page in your browser.

  Live from the Pi : python dashboard.py --pi 192.168.1.50
  Rehearse (fake)  : python dashboard.py --demo            (cycles normal -> attack -> flood)
  Replay real log  : python dashboard.py --replay gateway.log   (copy /tmp/gateway.log from the Pi)

Then open http://localhost:8080  (it opens by itself).
"""
import argparse, json, socket, threading, time, random, webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HIST_MAX = 600          # seconds of history kept
lock = threading.Lock()
hist = deque(maxlen=HIST_MAX)
S = {"mode": "live", "connected": False, "last_rx": 0.0, "latest": None, "final": None,
     "prev": None, "target": ""}


def derive(rec, prev):
    """Per-second numbers computed from the Gateway's cumulative counters."""
    sf, tl, bl = rec["safety"], rec["telem"], rec["blocked"]
    p = prev or {"safety": {"n": 0, "sum": 0}, "telem": {"n": 0, "sum": 0},
                 "blocked": {"allowlist": 0, "spoof": 0, "rate": 0, "implausible": 0, "rule": 0}}
    dsn = sf["n"] - p["safety"]["n"]
    dtn = tl["n"] - p["telem"]["n"]
    return {
        "t": rec["t"],
        "s_avg": (sf["sum"] - p["safety"]["sum"]) / dsn if dsn > 0 else None,
        "s_max": sf["win_max"] if dsn > 0 else None,
        "s_rate": dsn,
        "t_avg": (tl["sum"] - p["telem"]["sum"]) / dtn if dtn > 0 else None,
        "t_rate": dtn,
        "bus": rec["bus"]["load"],
        "b_allow": bl["allowlist"] - p["blocked"]["allowlist"],
        "b_spoof": bl["spoof"] - p["blocked"]["spoof"],
        "b_rate": bl["rate"] - p["blocked"]["rate"],
        "b_impl": bl["implausible"] - p["blocked"]["implausible"],
    }


def ingest(rec):
    if "safety" not in rec:          # event lines in the log (heartbeat_lost ...)
        return
    with lock:
        prev = S["prev"]
        if prev and (rec["t"] < prev["t"] or rec["safety"]["n"] < prev["safety"]["n"]):
            hist.clear(); S["final"] = None; prev = None      # a new run started
        S["last_rx"] = time.time()
        if rec.get("final"):
            S["final"] = rec                                  # summary, not a chart point
            return
        hist.append(derive(rec, prev))
        S["prev"] = rec
        S["latest"] = rec
        S["final"] = None


# ------------------------------------------------------------------ sources
def live_loop(host, port):
    while True:
        try:
            with socket.create_connection((host, port), timeout=3) as s:
                s.settimeout(5)
                with lock:
                    S["connected"] = True
                buf = b""
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        try:
                            ingest(json.loads(line))
                        except ValueError:
                            pass
        except OSError:
            pass
        with lock:
            S["connected"] = False
        time.sleep(1)


def replay_loop(path, speed):
    with open(path, "r", errors="replace") as f:
        lines = f.readlines()
    with lock:
        S["connected"] = True
    for ln in lines:
        try:
            rec = json.loads(ln)
        except ValueError:
            continue
        ingest(rec)
        time.sleep(1.0 / speed)
    # keep the last picture on screen; "age" will grow and the page shows it is finished


def demo_loop(speed):
    """Synthetic feed, same JSON shape as the Gateway. Phases of 15 s: normal, attack, flood."""
    c = dict(sn=0, ssum=0, smiss=0, tn=0, tsum=0, dn=0, hb=0, sout=0, bout=0,
             allow=0, spoof=0, rate=0, impl=0, peak=0.0)
    slat = []
    smax_all = tmax_all = 0
    t = 0
    flood_k = 0
    with lock:
        S["connected"] = True
    while True:
        time.sleep(1.0 / speed)
        t += 1
        phase = ((t - 1) // 15) % 3          # 0 normal, 1 attack, 2 flood
        flood_k = flood_k + 1 if phase == 2 else 0
        n_s = 20
        lats = [max(45, int(random.gauss(90 if phase == 2 else 85, 8))) for _ in range(n_s)]
        if random.random() < 0.15:
            lats[0] += random.randint(30, 90)
        slat += lats
        c["sn"] += n_s; c["ssum"] += sum(lats)
        smax_all = max(smax_all, max(lats))
        win_max = max(lats)
        n_t = 1000 if phase == 2 else 100
        tl = [max(60, int(random.gauss(230 if phase == 2 else 110, 25))) for _ in range(n_t)]
        c["tn"] += n_t; c["tsum"] += sum(tl); tmax_all = max(tmax_all, max(tl))
        d_in = 4000 if phase == 2 else 10
        d_ok = min(d_in, 3000)
        c["dn"] += d_ok
        c["rate"] += d_in - d_ok
        if phase == 1:
            c["allow"] += 100; c["spoof"] += 20; c["impl"] += 20
        c["hb"] += 5
        c["sout"] += n_s; c["bout"] += n_t + d_ok
        frames = n_s + n_t + d_in + 5 + (140 if phase == 1 else 0)
        load = frames * 111 / 500000 * 100
        c["peak"] = max(c["peak"], load)
        davg = 430 if phase != 2 else min(138000, 30000 + 20000 * flood_k)
        sl = sorted(slat)
        p99 = sl[min(len(sl) - 1, (99 * len(sl) + 99) // 100 - 1)]
        speed_v = 80 + int(8 * (1 if t % 2 else -1)); rpm = 2300; temp = 855
        rec = {"t": t, "final": 0, "prio": 1,
               "safety": {"n": c["sn"], "sum": c["ssum"], "avg": c["ssum"] // c["sn"], "p99": p99,
                          "max": smax_all, "win_max": win_max, "miss": 0},
               "telem": {"n": c["tn"], "sum": c["tsum"], "avg": c["tsum"] // c["tn"], "max": tmax_all,
                         "win_max": 0, "miss": 0, "speed": speed_v, "rpm": rpm, "temp": temp},
               "diag": {"n": c["dn"], "avg": davg, "max": int(davg * 1.7)},
               "drops": {"telem": 0, "diag": 0, "safety": 0},
               "blocked": {"allowlist": c["allow"], "spoof": c["spoof"], "rate": c["rate"],
                           "implausible": c["impl"], "rule": 0},
               "bus": {"load": round(load, 1), "peak": round(c["peak"], 1)},
               "hb": {"ok": 1, "count": c["hb"], "max_gap": 200},
               "egress": {"safety": c["sout"], "bulk": c["bout"], "fail": 0, "safety_up": 1, "bulk_up": 1}}
        ingest(rec)


# ------------------------------------------------------------------ web server
class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/data"):
            with lock:
                age = time.time() - S["last_rx"] if S["last_rx"] else None
                body = json.dumps({"mode": S["mode"], "connected": S["connected"], "age": age,
                                   "target": S["target"], "latest": S["latest"], "final": S["final"],
                                   "history": list(hist)[-120:]}).encode()
            ctype = "application/json"
        else:
            body = PAGE.encode(); ctype = "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Vehicle Gateway Dashboard</title>
<style>
:root{--bg:#f4f6f9;--card:#fff;--ink:#14181f;--mute:#5d6673;--line:#e3e6eb;--blue:#2563eb;--orange:#d97706;--red:#dc2626;--green:#16a34a;--purple:#7c3aed;--teal:#0d9488}
@media(prefers-color-scheme:dark){:root{--bg:#0e1116;--card:#171b22;--ink:#e6e9ee;--mute:#98a2b0;--line:#2a313c;--blue:#60a5fa;--orange:#fbbf24;--red:#f87171;--green:#4ade80;--purple:#a78bfa;--teal:#2dd4bf}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.4 system-ui,Segoe UI,Roboto,sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:16px}
header{display:flex;flex-wrap:wrap;gap:10px 16px;align-items:center;justify-content:space-between;margin-bottom:14px}
h1{font-size:18px;margin:0;font-weight:650}
.sub{color:var(--mute);font-size:12px}
.pill{display:inline-block;padding:3px 10px;border-radius:99px;font-size:12px;font-weight:650;color:#fff;background:var(--mute)}
.pill.live{background:var(--green)}.pill.off{background:var(--red)}.pill.demo{background:var(--purple)}.pill.done{background:var(--blue)}
.grid{display:grid;gap:12px}
.kpis{grid-template-columns:repeat(auto-fit,minmax(170px,1fr));margin-bottom:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.k{color:var(--mute);font-size:12px;margin-bottom:4px}
.v{font-size:26px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums}
.v small{font-size:13px;font-weight:500;color:var(--mute);margin-left:3px}
.d{color:var(--mute);font-size:12px;margin-top:2px;font-variant-numeric:tabular-nums}
.good{color:var(--green)}.bad{color:var(--red)}
.charts{grid-template-columns:repeat(auto-fit,minmax(340px,1fr));margin-bottom:12px}
.ch h2{font-size:13px;margin:0 0 2px;font-weight:650}
.ro{color:var(--mute);font-size:12px;min-height:18px;font-variant-numeric:tabular-nums}
canvas{width:100%;height:190px;display:block}
.leg{display:flex;gap:12px;flex-wrap:wrap;font-size:12px;color:var(--mute);margin-top:4px}
.sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:4px;vertical-align:-1px}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
td{padding:4px 0;border-bottom:1px solid var(--line)}td:last-child{text-align:right;font-weight:650}
tr:last-child td{border-bottom:0}
.note{color:var(--mute);font-size:12px;margin-top:8px}
</style></head><body><div class="wrap">
<header>
  <div><h1>Vehicle Gateway &middot; Safety-Critical CAN Scheduling</h1>
  <div class="sub" id="hsub">&nbsp;</div></div>
  <div><span class="pill" id="pill">connecting</span></div>
</header>

<div class="grid kpis" id="kpis"></div>

<div class="grid charts">
  <div class="card ch"><h2>Safety latency per second (source &rarr; out)</h2><div class="ro" id="r1"></div>
    <canvas id="c1"></canvas>
    <div class="leg"><span><i class="sw" style="background:var(--blue)"></i>average</span><span><i class="sw" style="background:var(--orange)"></i>worst in second</span><span><i class="sw" style="background:var(--red)"></i>500 &micro;s deadline</span></div></div>
  <div class="card ch"><h2>Bus load (% of simulated 500 kbps CAN)</h2><div class="ro" id="r2"></div>
    <canvas id="c2"></canvas>
    <div class="leg"><span><i class="sw" style="background:var(--purple)"></i>ingest load</span><span><i class="sw" style="background:var(--red)"></i>100% = bus full</span></div></div>
  <div class="card ch"><h2>Frames blocked per second (security)</h2><div class="ro" id="r3"></div>
    <canvas id="c3"></canvas>
    <div class="leg"><span><i class="sw" style="background:var(--orange)"></i>unknown ID</span><span><i class="sw" style="background:var(--red)"></i>spoofed type</span><span><i class="sw" style="background:var(--purple)"></i>rate limited</span><span><i class="sw" style="background:var(--teal)"></i>implausible value</span></div></div>
  <div class="card"><div class="k">Security counters (since start of run)</div><table id="sec"></table>
    <div class="note">Allowlist = default deny. Spoofed = ID does not match message type. Rate limit = per-ID token bucket. Implausible = value outside configured limits.</div></div>
</div>
<div class="sub">Latency is measured with CLOCK_MONOTONIC from the Sender's timestamp to the Gateway's output. The CAN bus is simulated; Sender, Gateway and Receiver talk over QNX message passing.</div>
</div>
<script>
const N=120;let J=null,hist=[];const hov={};
const $=id=>document.getElementById(id);
const css=()=>getComputedStyle(document.documentElement);
function fmt(v,u){if(v==null)return'–';return(Number.isInteger(v)||v>=100?Math.round(v):v.toFixed(1))+(u||'')}
function ms(us){return us>=1000?(us/1000).toFixed(1)+' ms':Math.round(us)+' µs'}
function niceMax(v){if(v<=0)return 1;const p=Math.pow(10,Math.floor(Math.log10(v)));const m=v/p;return(m<=1?1:m<=2?2:m<=5?5:10)*p}
function ax(v){return v>=1000?(v/1000)+'k':(v%1===0?v:v.toFixed(1))}

const CH=[
 {id:'c1',ro:'r1',ref:500,series:[{k:'s_avg',c:'--blue',l:'avg',u:' µs'},{k:'s_max',c:'--orange',l:'max',u:' µs'}]},
 {id:'c2',ro:'r2',ref:100,series:[{k:'bus',c:'--purple',l:'load',u:' %'}]},
 {id:'c3',ro:'r3',stack:true,series:[{k:'b_allow',c:'--orange',l:'unknown ID'},{k:'b_spoof',c:'--red',l:'spoofed'},{k:'b_rate',c:'--purple',l:'rate-limited'},{k:'b_impl',c:'--teal',l:'implausible'}]}
];

function drawChart(d){
  const c=$(d.id),dpr=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
  if(c.width!==Math.round(w*dpr)){c.width=Math.round(w*dpr);c.height=Math.round(h*dpr)}
  const g=c.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);g.clearRect(0,0,w,h);
  const st=css(),col=n=>st.getPropertyValue(n).trim();
  const L=40,R=6,T=6,B=16,pw=w-L-R,ph=h-T-B,sw=pw/N;
  const pts=hist.slice(-N),n=pts.length,off=N-n;
  let vmax=d.ref||0;
  for(const p of pts){if(d.stack){let s=0;for(const s_ of d.series)s+=p[s_.k]||0;vmax=Math.max(vmax,s)}
    else for(const s_ of d.series){const v=p[s_.k];if(v!=null)vmax=Math.max(vmax,v)}}
  vmax=niceMax(vmax*1.05);
  const X=i=>L+(off+i+.5)*sw,Y=v=>T+ph-v/vmax*ph;
  g.font='11px system-ui';g.lineWidth=1;g.textAlign='right';
  for(let k=0;k<=4;k++){const v=vmax*k/4,y=Math.round(Y(v))+.5;
    g.strokeStyle=col('--line');g.beginPath();g.moveTo(L,y);g.lineTo(w-R,y);g.stroke();
    g.fillStyle=col('--mute');g.fillText(ax(v),L-5,y+4)}
  g.textAlign='left';g.fillStyle=col('--mute');g.fillText('-'+N+' s',L,h-3);g.textAlign='right';g.fillText('now',w-R,h-3);
  if(d.ref){const y=Math.round(Y(d.ref))+.5;g.strokeStyle=col('--red');g.setLineDash([5,4]);g.beginPath();g.moveTo(L,y);g.lineTo(w-R,y);g.stroke();g.setLineDash([])}
  if(d.stack){
    pts.forEach((p,i)=>{let base=0;for(const s_ of d.series){const v=p[s_.k]||0;if(!v)continue;
      g.fillStyle=col(s_.c);const y1=Y(base+v),y0=Y(base);g.fillRect(X(i)-sw*.4,y1,sw*.8,Math.max(1,y0-y1));base+=v}});
  }else for(const s_ of d.series){
    g.strokeStyle=col(s_.c);g.lineWidth=2;g.lineJoin='round';g.beginPath();let pen=false;
    pts.forEach((p,i)=>{const v=p[s_.k];if(v==null){pen=false;return}
      pen?g.lineTo(X(i),Y(v)):g.moveTo(X(i),Y(v));pen=true});g.stroke();
    if(n===1||(pts.length&&pts[n-1][s_.k]!=null)){const v=pts[n-1][s_.k];g.fillStyle=col(s_.c);g.beginPath();g.arc(X(n-1),Y(v),3,0,7);g.fill()}
  }
  let hi=hov[d.id];if(hi!=null&&hi>=0&&hi<n){g.strokeStyle=col('--mute');g.lineWidth=1;g.beginPath();g.moveTo(X(hi),T);g.lineTo(X(hi),T+ph);g.stroke()}
  const p=pts[hi!=null&&hi>=0&&hi<n?hi:n-1];
  $(d.ro).textContent=p?('t='+p.t+' s   '+d.series.map(s_=>s_.l+' '+fmt(p[s_.k],s_.u||'')).join('  ·  ')):'waiting for data…';
}
CH.forEach(d=>{const c=$(d.id);
  c.addEventListener('mousemove',e=>{const r=c.getBoundingClientRect(),sw=(r.width-46)/N,n=Math.min(hist.length,N);
    hov[d.id]=Math.floor((e.clientX-r.left-40)/sw)-(N-n);drawChart(d)});
  c.addEventListener('mouseleave',()=>{hov[d.id]=null;drawChart(d)})});

function tile(k,v,d,cls){return '<div class="card"><div class="k">'+k+'</div><div class="v '+(cls||'')+'">'+v+'</div><div class="d">'+d+'</div></div>'}
function render(){
  if(!J)return;hist=J.history;
  const r=J.final||J.latest,last=hist[hist.length-1];
  const pill=$('pill');let txt,cls;
  const stale=J.age!=null&&J.age>4;
  if(J.mode==='demo'){txt='DEMO · synthetic data';cls='demo'}
  else if(J.final){txt='RUN COMPLETE · final summary';cls='done'}
  else if(J.mode==='replay'){txt=stale?'REPLAY finished':'REPLAY';cls=stale?'done':'demo'}
  else if(!J.connected||stale){txt=(J.latest?'GATEWAY OFFLINE · showing last data':'WAITING FOR GATEWAY');cls='off'}
  else{txt='LIVE';cls='live'}
  pill.textContent=txt;pill.className='pill '+cls;
  $('hsub').textContent=(J.target?J.target+' · ':'')+(r?('priorities '+(r.prio?'ON (SCHED_FIFO)':'OFF (all equal)')+' · t='+r.t+' s'):'no data yet');
  if(!r){$('kpis').innerHTML=tile('Waiting','–','start the Gateway and Sender on the Pi');drawAll();return}
  const sf=r.safety,miss=sf.miss,hb=r.hb,eg=r.egress,dr=r.drops;
  const drops=dr.safety+dr.telem+dr.diag;
  const sAvg=last&&last.s_avg!=null?last.s_avg:sf.avg;
  $('kpis').innerHTML=
   tile('Safety latency (now)',fmt(sAvg)+'<small>µs</small>','p99 '+sf.p99+' µs · max '+sf.max+' µs',miss?'bad':'good')+
   tile('Safety deadline misses',miss,'deadline 500 µs · '+sf.n+' frames',miss?'bad':'good')+
   tile('Heartbeat',hb.ok?'OK':'LOST',hb.count+' beats · max gap '+hb.max_gap+' ms',hb.ok?'good':'bad')+
   tile('Bus load',r.bus.load.toFixed(1)+'<small>%</small>','peak '+r.bus.peak.toFixed(1)+'%',r.bus.load>100?'bad':'')+
   tile('Dropped frames',drops,'safety '+dr.safety+' · telemetry '+dr.telem+' · diag '+dr.diag,dr.safety?'bad':'good')+
   tile('Telemetry',r.telem.n,'avg '+ms(r.telem.avg)+' · max '+ms(r.telem.max)+' · miss '+r.telem.miss)+
   tile('Diagnostics',r.diag.n,'avg '+ms(r.diag.avg)+' · max '+ms(r.diag.max))+
   tile('Forwarded to receiver',(eg.safety+eg.bulk),'safety '+eg.safety+' · bulk '+eg.bulk+' · fail '+eg.fail+' · links '+(eg.safety_up&&eg.bulk_up?'up':'DOWN'),(eg.safety_up&&eg.bulk_up)?'':'bad')+
   tile('Vehicle (last good telemetry)',r.telem.speed+'<small>km/h</small>',r.telem.rpm+' rpm · '+(r.telem.temp/10).toFixed(1)+' °C');
  const b=r.blocked,tot=b.allowlist+b.spoof+b.rate+b.implausible+b.rule;
  $('sec').innerHTML=[['Unknown ID (not on allowlist)',b.allowlist],['Spoofed (ID/type mismatch)',b.spoof],['Rate limited',b.rate],['Implausible value',b.implausible],['Blocked by rule',b.rule],['Total blocked',tot]]
    .map(x=>'<tr><td>'+x[0]+'</td><td>'+x[1]+'</td></tr>').join('');
  drawAll();
}
function drawAll(){CH.forEach(drawChart)}
async function poll(){try{const r=await fetch('/data',{cache:'no-store'});J=await r.json();render()}
  catch(e){$('pill').textContent='dashboard server stopped';$('pill').className='pill off'}}
setInterval(poll,500);poll();addEventListener('resize',drawAll);
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser(description="Vehicle Gateway dashboard")
    ap.add_argument("--pi", help="Pi IP address (live mode)")
    ap.add_argument("--stats-port", type=int, default=6100)
    ap.add_argument("--demo", action="store_true", help="synthetic data, no Pi needed")
    ap.add_argument("--replay", metavar="LOGFILE", help="replay a gateway.log copied from the Pi")
    ap.add_argument("--speed", type=float, default=1.0, help="demo/replay speed-up factor")
    ap.add_argument("--port", type=int, default=8080, help="dashboard web port")
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    if not (a.pi or a.demo or a.replay):
        ap.error("give --pi <ip>, or --demo, or --replay <file>")

    if a.demo:
        S["mode"] = "demo"; S["target"] = "synthetic feed"
        threading.Thread(target=demo_loop, args=(a.speed,), daemon=True).start()
    elif a.replay:
        S["mode"] = "replay"; S["target"] = a.replay
        threading.Thread(target=replay_loop, args=(a.replay, a.speed), daemon=True).start()
    else:
        S["target"] = "Gateway @ %s:%d" % (a.pi, a.stats_port)
        threading.Thread(target=live_loop, args=(a.pi, a.stats_port), daemon=True).start()

    srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    url = "http://localhost:%d" % a.port
    print("Dashboard running at", url, " (Ctrl+C to stop)")
    if not a.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
