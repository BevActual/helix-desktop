import sys, json, time, os
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from routing_set import SET
import gi
gi.require_version("Gio","2.0")
from gi.repository import Gio, GLib
conn = Gio.bus_get_sync(Gio.BusType.SESSION)
ROUTES=[{"name":"code","description":"Anything about programming: writing, fixing, explaining or reviewing code, regular expressions, SQL, shell commands and command line how-to"},
        {"name":"desktop_action","description":"A request to change something on the computer: volume, brightness, theme, opening an app, settings, notifications"},
        {"name":"deep_reasoning","description":"Non-programming questions that need careful multi-step reasoning: analysis, planning, long-form writing, maths, comparisons"},
        {"name":"quick_answer","description":"A short factual or conversational answer, a rewrite, a translation, small talk"}]
MAP={"quick_answer":"quick","deep_reasoning":"deep","code":"code","desktop_action":"act"}
TASK="""
You are a helpful assistant designed to find the best suited route.
You are provided with route description within <routes></routes> XML tags:
<routes>
{routes}
</routes>
<conversation>
{conversation}
</conversation>
"""
FMT="""
Your task is to decide which route is best suit with user intent on the conversation in <conversation></conversation> XML tags.  Follow the instruction:
1. If the latest intent from user is irrelevant or user intent is full filled, response with other route {"route": "other"}.
2. You must analyze the route descriptions and find the best match route for user latest intent. 
3. You only response the name of the route that best matches the user's request, use the exact name in the <routes></routes>.
Based on your analysis, provide your response in the following JSON formats if you decide to match any route:
{"route": "route_name"} 
"""
def v(x):
    if isinstance(x,dict): return GLib.Variant("a{sv}",{k:v(i) for k,i in x.items()})
    if isinstance(x,list): return GLib.Variant("av",[v(i) for i in x])
    if isinstance(x,str): return GLib.Variant("s",x)
    if isinstance(x,float): return GLib.Variant("d",x)
    return GLib.Variant("u",x)
model=sys.argv[1]
def raw(text):
    prompt=TASK.format(routes=json.dumps(ROUTES),conversation=json.dumps([{"role":"user","content":text}]))+FMT
    opts={"messages":[{"role":"user","content":prompt}],"task":"chat","model":model,"temperature":0.0,"max_tokens":24,"priority":"interactive"}
    loop=GLib.MainLoop(); out=[]
    def on(c,s,p,i,n,params): out.append(params.unpack()); loop.quit()
    sub=conn.signal_subscribe("org.helix.Inference","org.helix.Inference1","Completed","/org/helix/Inference",None,Gio.DBusSignalFlags.NONE,on)
    conn.call_sync("org.helix.Inference","/org/helix/Inference","org.helix.Inference1","Action",GLib.Variant("(sa{sv})",("chat",{k:v(x) for k,x in opts.items()})),None,0,20000,None)
    GLib.timeout_add_seconds(120,loop.quit); loop.run(); conn.signal_unsubscribe(sub)
    return out[0] if out else None
def ask(text):
    prompt=TASK.format(routes=json.dumps(ROUTES),conversation=json.dumps([{"role":"user","content":text}]))+FMT
    opts={"messages":[{"role":"user","content":prompt}],"task":"chat","model":model,"temperature":0.0,"max_tokens":24,"priority":"interactive"}
    loop=GLib.MainLoop(); out=[]
    def on(c,s,p,i,n,params): out.append(params.unpack()[2]); loop.quit()
    sub=conn.signal_subscribe("org.helix.Inference","org.helix.Inference1","Completed","/org/helix/Inference",None,Gio.DBusSignalFlags.NONE,on)
    t=time.time()
    conn.call_sync("org.helix.Inference","/org/helix/Inference","org.helix.Inference1","Action",GLib.Variant("(sa{sv})",("chat",{k:v(x) for k,x in opts.items()})),None,0,20000,None)
    GLib.timeout_add_seconds(120,loop.quit); loop.run(); conn.signal_unsubscribe(sub)
    dt=time.time()-t
    try:
        import re; txt=json.loads(out[0])["text"]; r=re.search(r"route['\"]\s*:\s*['\"]([A-Za-z_]+)",txt).group(1)
        return MAP.get(r,r), dt
    except Exception: return "?", dt
import sys as _s
if len(_s.argv)>2:
    for t,w in SET[:4]:
        prompt=TASK.format(routes=json.dumps(ROUTES),conversation=json.dumps([{"role":"user","content":t}]))+FMT
        print(repr(t), "->", repr(raw(t)))
    raise SystemExit
ask("warm up")
right=0; times=[]; bad=[]
for text,want in SET:
    got,dt=ask(text); times.append(dt)
    if got==want: right+=1
    else: bad.append((text[:50],want,got))
times.sort()
print(f"{model}: {right}/{len(SET)} right, median {times[len(times)//2]*1000:.0f} ms, p90 {times[int(len(times)*0.9)]*1000:.0f} ms")
for b in bad: print("  miss:",b)
