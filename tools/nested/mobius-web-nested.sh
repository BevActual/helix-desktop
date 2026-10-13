#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/web-fixtures" "$XDG_STATE_HOME/browser-bin"
cat > "$XDG_STATE_HOME/web-fixtures/search.xml" <<'XML'
<rss><channel><item><title>Official guide</title><link>https://example.com/guide</link><description>A public guide explains local tools and source evidence.</description></item><item><title>Reference</title><link>https://example.org/reference</link><description>A separate search snippet gives supporting information.</description></item></channel></rss>
XML
cat > "$XDG_STATE_HOME/web-fixtures/page.html" <<'HTML'
<title>Mobius web test</title><nav>Navigation text</nav><main><h1>Public web guide</h1><p>The guide describes source evidence from public websites. Retrieved text remains separate from executable actions.</p><script>DO_NOT_INCLUDE_SCRIPT</script><div hidden>DO_NOT_INCLUDE_HIDDEN</div></main>
HTML
cat > "$XDG_STATE_HOME/browser-bin/xdg-open" <<'BROWSER'
#!/bin/bash
printf '%s\n' "$1" >> "$XDG_STATE_HOME/opened-sources.log"
BROWSER
chmod +x "$XDG_STATE_HOME/browser-bin/xdg-open"
export PATH="$XDG_STATE_HOME/browser-bin:$PATH" HELIX_WEB_TEST_FIXTURES="$XDG_STATE_HOME/web-fixtures"
exec "$ROOT/tools/nested/mobius-nested.sh"
