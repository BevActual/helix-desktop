import sys, json, time, os
import gi
gi.require_version("Gio","2.0")
from gi.repository import Gio, GLib
conn = Gio.bus_get_sync(Gio.BusType.SESSION)
SYS = ("Classify the user's message for routing. Reply with exactly one JSON tool call. Tools: "
 "- quick: a short factual or conversational answer, a rewrite, a translation, small talk.\n"
 "- deep: needs careful multi-step reasoning, analysis, planning, long writing, maths or comparison.\n"
 "- code: writing, fixing, explaining or reviewing source code or shell commands.\n"
 "- act: asks to change something on the computer (volume, brightness, theme, open an app, settings).")
SET = [
 ("What's the capital of Australia?","quick"),("Translate 'good night' to French","quick"),("hi there!","quick"),("Rewrite this to sound friendlier: send me the report","quick"),
 ("How tall is Mount Everest?","quick"),("Give me a synonym for happy","quick"),("What does HTTP stand for?","quick"),("thanks","quick"),("Who wrote Dune?","quick"),("Spell 'necessary'","quick"),
 ("Compare PostgreSQL and SQLite for a small desktop app and recommend one with reasons","deep"),("Plan a 5-day study schedule for learning linear algebra","deep"),("Explain why the sky is blue and why sunsets are red, step by step","deep"),
 ("Write a 500 word essay on the causes of the French Revolution","deep"),("If a train leaves at 14:20 and arrives at 17:05, and the next leaves 40 minutes later, when does the second arrive if it is 12% faster?","deep"),
 ("What are the trade-offs of microservices versus a monolith for a team of three?","deep"),("Draft a project proposal for a local-first note taking app","deep"),("Summarise the pros and cons of remote work for a small company","deep"),
 ("Write a python function that merges two sorted lists","code"),("Why does this throw a KeyError: d['a']['b'] when d={'a':{}}","code"),("Convert this bash loop to a one liner with xargs","code"),("What does `git rebase -i HEAD~3` do?","code"),
 ("Fix the off by one error in my for loop in rust","code"),("Write a regex that matches ISO dates","code"),("Explain this SQL: select * from t group by 1 having count(*)>1","code"),("how do i list the 5 largest files with du","code"),
 ("Turn the volume down","act"),("Make the screen dimmer","act"),("Switch to dark mode","act"),("Open the settings","act"),("Turn on do not disturb","act"),("Set brightness to 40 percent","act"),("Mute the microphone","act"),("Open the software store","act"),
]
model = sys.argv[1]
def v(x):
    if isinstance(x,dict): return GLib.Variant("a{sv}",{k:v(i) for k,i in x.items()})
    if isinstance(x,list): return GLib.Variant("av",[v(i) for i in x])
    if isinstance(x,str): return GLib.Variant("s",x)
    if isinstance(x,float): return GLib.Variant("d",x)
    return GLib.Variant("u",x)
schema = {"oneOf":[{"type":"object","properties":{"tool":{"const":t},"args":{"type":"array","minItems":0,"maxItems":0,"items":False}},"required":["tool","args"],"additionalProperties":False} for t in ("quick","deep","code","act")]}
def ask(text):
    opts={"messages":[{"role":"system","content":SYS},{"role":"user","content":text}],"schema":json.dumps(schema),"task":"act","model":model,"temperature":0.0,"max_tokens":16,"priority":"interactive"}
    loop=GLib.MainLoop(); out=[]
    def on(c,s,p,i,n,params): out.append(params.unpack()[2]); loop.quit()
    sub=conn.signal_subscribe("org.helix.Inference","org.helix.Inference1","Completed","/org/helix/Inference",None,Gio.DBusSignalFlags.NONE,on)
    t=time.time()
    conn.call_sync("org.helix.Inference","/org/helix/Inference","org.helix.Inference1","Action",GLib.Variant("(sa{sv})",("chat",{k:v(x) for k,x in opts.items()})),None,0,20000,None)
    GLib.timeout_add_seconds(120,loop.quit); loop.run(); conn.signal_unsubscribe(sub)
    dt=time.time()-t
    try: return json.loads(json.loads(out[0])["text"])["tool"], dt
    except Exception: return "?", dt
ask("warm up")
right=0; times=[]; bad=[]
for text,want in SET:
    got,dt=ask(text); times.append(dt)
    if got==want: right+=1
    else: bad.append((text[:50],want,got))
times.sort()
print(f"{model}: {right}/{len(SET)} right, median {times[len(times)//2]*1000:.0f} ms, p90 {times[int(len(times)*0.9)]*1000:.0f} ms")
for b in bad: print("  miss:",b)
