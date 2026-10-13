#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/web-fixtures" "$XDG_STATE_HOME/tools-bin" "$HOME/project"
cat > "$XDG_STATE_HOME/web-fixtures/search.xml" <<'XML'
<rss><channel><item><title>Guide</title><link>https://example.com/guide</link><description>Tools use public evidence.</description></item></channel></rss>
XML
printf '<main><h1>Guide</h1><p>Public source evidence helps answer research questions.</p></main>' > "$XDG_STATE_HOME/web-fixtures/page.html"
cat > "$XDG_STATE_HOME/tools-bin/zenity" <<'PICK'
#!/bin/bash
printf '%s/project\n' "$HOME"
PICK
chmod +x "$XDG_STATE_HOME/tools-bin/zenity"
cat > "$XDG_STATE_HOME/tools-steps.json" <<'JSON'
[{"tool":"web_search","query":"fixture topic"},{"tool":"web_read","url":"https://example.com/guide"},{"tool":"terminal","command":"printf reviewed > proof.txt; cat proof.txt"},{"tool":"finish"}]
JSON
export PATH="$XDG_STATE_HOME/tools-bin:$PATH" HELIX_WEB_TEST_FIXTURES="$XDG_STATE_HOME/web-fixtures" HELIX_TOOLS_TEST_RESPONSES="$XDG_STATE_HOME/tools-steps.json"
exec "$ROOT/tools/nested/mobius-nested.sh"
