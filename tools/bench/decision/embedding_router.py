import json, subprocess, time, numpy as np
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from routing_set import SET
ROOT=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"../../.."))
BIN=ROOT+"/third_party/llama-install/bin/llama-embedding"
MODEL=os.environ.get("EMBED_MODEL",os.path.expanduser("~/.cache/helix-experiments/models/nomic-embed-text-v1.5.Q8_0.gguf"))
TRAIN={
 "quick":["What's the population of Canada?","Say hello in Spanish","Define the word ephemeral","Who painted the Mona Lisa?","Make this sentence shorter: I would like to go to the store","good morning","what year did WW2 end","Translate 'thank you' into German","How many days are in February?","Tell me a fun fact"],
 "deep":["Compare React and Vue for a large team and give a recommendation","Plan a three month training schedule for a first marathon","Explain how inflation works and why central banks raise rates","Write a detailed essay about the industrial revolution","Work out how long a trip takes with these stops and speeds","Analyse the strengths and weaknesses of this business idea","Outline a strategy for learning Japanese in a year","What are the pros and cons of nuclear power, step by step?"],
 "code":["Write a function in javascript that debounces another function","Why is my python list comprehension returning None","Explain what this awk command does","How do I undo my last git commit","Fix the borrow checker error in this rust code","Give me a sql query to find duplicate emails","Write a bash script that renames files to lowercase","What is the difference between a mutex and a semaphore in C"],
 "act":["Lower the volume","Make the display brighter","Enable dark theme","Launch the file manager","Silence notifications","Increase screen brightness to 80","Turn off the microphone","Open the system monitor"],
}
def embed(texts, prefix="classification: "):
    sep="<#sep#>"
    t=time.time()
    out=subprocess.run([BIN,"-m",MODEL,"-ngl","0","--pooling","mean","--embd-normalize","2","--embd-output-format","json","--embd-separator",sep,"-p",sep.join(prefix+x for x in texts),"-c","512"],capture_output=True,text=True,env={"LD_LIBRARY_PATH":ROOT+"/third_party/llama-install/lib64","PATH":"/usr/bin"})
    d=json.loads(out.stdout[out.stdout.index("{"):])
    return np.array([e["embedding"] for e in d["data"]]), time.time()-t
labels=[];texts=[]
for k,v in TRAIN.items():
    for x in v: labels.append(k); texts.append(x)
X,tt=embed(texts); print("embedded",len(texts),"training examples in",round(tt,2),"s")
Y,tq=embed([t for t,_ in SET]); print("embedded",len(SET),"test prompts in",round(tq,2),"s (process start included)")
cent={k:X[[i for i,l in enumerate(labels) if l==k]].mean(0) for k in TRAIN}
for k in cent: cent[k]/=np.linalg.norm(cent[k])
right=0;bad=[];right_knn=0
for (t,w),y in zip(SET,Y):
    sims={k:float(y@c) for k,c in cent.items()}; got=max(sims,key=sims.get)
    s=sorted(sims.values(),reverse=True); margin=s[0]-s[1]
    if got==w: right+=1
    else: bad.append((t[:48],w,got,round(margin,3)))
    # kNN k=3
    order=np.argsort(-(X@y))[:3]; votes=[labels[i] for i in order]; knn=max(set(votes),key=votes.count)
    if knn==w: right_knn+=1
print(f"centroid: {right}/{len(SET)} right; 3-NN: {right_knn}/{len(SET)} right")
for b in bad: print("  miss:",b)
