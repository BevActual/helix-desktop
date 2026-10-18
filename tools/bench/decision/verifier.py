import sys, json, time, tomllib, re, os
import gi
gi.require_version("Gio","2.0")
from gi.repository import Gio, GLib
conn = Gio.bus_get_sync(Gio.BusType.SESSION)
hints = tomllib.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"../../../protocols/ipc/tool-hints.toml"),"rb"))["hints"]
rows = json.load(open(os.environ.get("EVAL_JSON", "/tmp/eval-7b-main.json")))["rows"]
model = sys.argv[1]
SYS = ("You check an assistant's work. A person asked for something on their Linux desktop. The assistant chose one command. "
       "Answer yes only if running that command does what was asked, not something merely related, and not just a status check. Reply with one JSON tool call.")
def v(x):
    if isinstance(x,dict): return GLib.Variant("a{sv}",{k:v(i) for k,i in x.items()})
    if isinstance(x,list): return GLib.Variant("av",[v(i) for i in x])
    if isinstance(x,str): return GLib.Variant("s",x)
    if isinstance(x,float): return GLib.Variant("d",x)
    return GLib.Variant("u",x)
schema = {"oneOf":[{"type":"object","properties":{"tool":{"const":t},"args":{"type":"array","minItems":0,"maxItems":0,"items":False}},"required":["tool","args"],"additionalProperties":False} for t in ("yes","no")]}
def ask(text):
    opts={"messages":[{"role":"system","content":SYS},{"role":"user","content":text}],"schema":json.dumps(schema),"task":"act","model":model,"temperature":0.0,"max_tokens":32,"priority":"interactive"}
    loop=GLib.MainLoop(); out=[]
    def on(c,s,p,i,n,params): out.append(params.unpack()[2]); loop.quit()
    sub=conn.signal_subscribe("org.helix.Inference","org.helix.Inference1","Completed","/org/helix/Inference",None,Gio.DBusSignalFlags.NONE,on)
    t=time.time()
    conn.call_sync("org.helix.Inference","/org/helix/Inference","org.helix.Inference1","Action",GLib.Variant("(sa{sv})",("chat",{k:v(x) for k,x in opts.items()})),None,0,20000,None)
    GLib.timeout_add_seconds(120,loop.quit); loop.run(); conn.signal_unsubscribe(sub)
    try: return json.loads(json.loads(out[0])["text"])["tool"], time.time()-t
    except Exception: return "?", time.time()-t
ask("warm")
tp=fp=tn=fn=0; times=[]; wrong=[]
for r in rows:
    got=r["got"]
    if got=="decline" or got=="invalid": continue   # a decline needs no check
    tool=got.split(" ")[0]
    desc=hints.get(tool, tool)
    args=got[len(tool):].strip()
    text=f'Request: {r["request"]}\nChosen command: {tool} {args} ({desc})\nDoes this command do what was asked?'
    ans,dt=ask(text); times.append(dt)
    right=r["ok"]
    if right and ans=="yes": tp+=1
    elif right and ans=="no": fn+=1; wrong.append(("blocked a right call",r["request"][:45],got))
    elif (not right) and ans=="no": tn+=1
    else: fp+=1; wrong.append(("let a wrong call through",r["request"][:45],got))
times.sort()
print(f"{model}: of {tp+fn+tn+fp} proposed calls: right calls kept {tp}/{tp+fn}, wrong calls caught {tn}/{tn+fp}; median {times[len(times)//2]*1000:.0f} ms")
for w in wrong: print("  ",w)
