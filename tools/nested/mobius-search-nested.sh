#!/bin/bash
# Private history: more results than the search cap, with the newest fixture written last.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 - "$XDG_DATA_HOME" <<'PY'
import json,pathlib,sys
base=pathlib.Path(sys.argv[1])/'helix/mobius/chats';base.mkdir(parents=True,exist_ok=True)
for i in range(1,86):
    id=f'{i:016x}'
    chat={'version':1,'id':id,'title':f'Fixture {i}','updated':1760000000+i,'pinned':False,'project':'',
          'messages':[{'role':'user','text':'Compare saved answers','images':[]},
                      {'role':'assistant','text':f'Answer {i}: **quartz** appears here.','images':[]}]}
    if i==85: chat['title']='ÉCOLE latest'
    (base/f'{id}.json').write_text(json.dumps(chat))
PY
exec "$ROOT/tools/nested/mobius-nested.sh"
