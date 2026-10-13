#!/bin/bash
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 - "$XDG_DATA_HOME" <<'PY'
import pathlib,json,sys
p=pathlib.Path(sys.argv[1])/'helix/mobius/chats';p.mkdir(parents=True,exist_ok=True)
messages=[]
for i in range(10):
    messages.extend([{'role':'user','text':f'Question {i}: '+'q'*1000,'images':[]}, {'role':'assistant','text':f'Answer {i}: '+'a'*1000,'images':[]}])
chat={'version':1,'id':'00000000000000c1','title':'Long study','updated':1760000000,'pinned':False,'project':'','messages':messages}
(p/'00000000000000c1.json').write_text(json.dumps(chat))
PY
exec "$ROOT/tools/nested/mobius-nested.sh"
