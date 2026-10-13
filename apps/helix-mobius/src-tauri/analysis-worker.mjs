// Runs only inside the analysis sandbox. No chat text is evaluated as code or a command.
import { readFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";

const toolsRoot = process.env.HELIX_ANALYSIS_PACKAGES || "/tools";
const moduleAt = (path) => import(pathToFileURL(`${toolsRoot}/node_modules/${path}`).href);
const { Client } = await moduleAt("@modelcontextprotocol/client/dist/index.mjs");
const { StdioClientTransport } = await moduleAt("@modelcontextprotocol/client/dist/stdio.mjs");
const { default: Ajv } = await moduleAt("ajv/dist/2020.js");
const { default: addFormats } = await moduleAt("ajv-formats/dist/index.js");
const ajv = new Ajv({ strict: false, allErrors: true });
addFormats(ajv);
const selected = JSON.parse(await readFile(process.argv[2], "utf8"));
const emit = (value) => process.stdout.write(JSON.stringify(value) + "\n");
const ALLOWED = new Set(["open_binary", "close_binary", "binary_overview", "list_procedures", "list_strings", "analyze_function"]);
const records = [];
const client = new Client({ name: "helix-mobius-analysis", version: "0.1.0" }, { capabilities: {} });
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [`${toolsRoot}/node_modules/rea-agents/scripts/rea.mjs`, "mcp"],
  env: { ...process.env, REA_ANALYSIS_PROVIDER: "ghidra", REA_GHIDRA_STARTUP_TIMEOUT_MS: "240000" },
  stderr: "inherit",
  maxBufferSize: 10 * 1024 * 1024,
});
const definitions = new Map();
let opened = false;
let failure;

async function call(name, args, stage, timeout = 90000) {
  if (!ALLOWED.has(name)) throw new Error("Analysis tool is not allowed.");
  const tool = definitions.get(name);
  if (!tool) throw new Error(`REA does not advertise ${name}. Check the installed version.`);
  const validate = ajv.compile(tool.inputSchema);
  if (!validate(args)) throw new Error(`Invalid ${name} arguments: ${ajv.errorsText(validate.errors)}`);
  emit({ kind: "stage", stage });
  const result = await client.callTool({ name, arguments: args }, { timeout, resetTimeoutOnProgress: false });
  if (result.isError) {
    const diagnostic = (result.content || []).filter((c) => c.type === "text").map((c) => c.text).join("\n");
    throw new Error(`${name}: ${diagnostic.slice(0, 8000)}`);
  }
  if (name === "open_binary" || name === "close_binary") return result;
  const evidence = result.structuredContent;
  if (!evidence || !Object.hasOwn(evidence, "normalized_result") || !evidence.evidence_id) {
    throw new Error(`${name} returned no canonical evidence.`);
  }
  const record = { reference: `E${records.length + 1}`, tool: name, arguments: args, evidence };
  // A complete record is either retained or rejected; never quietly truncate it.
  if (Buffer.byteLength(JSON.stringify(record)) > 4 * 1024 * 1024) {
    throw new Error(`${name} evidence exceeds Mobius's 4 MiB record limit. Select a smaller artifact.`);
  }
  records.push(record);
  emit({ kind: "evidence", record });
  return evidence.normalized_result;
}

try {
  emit({ kind: "stage", stage: "Connecting to REA" });
  await client.connect(transport, { timeout: 30000 });
  emit({ kind: "engine", server: client.getServerVersion() });
  let cursor;
  for (let page = 0; page < 10; page++) {
    const listing = await client.listTools(cursor ? { cursor } : undefined, { timeout: 30000 });
    for (const tool of listing.tools) if (ALLOWED.has(tool.name)) definitions.set(tool.name, tool);
    cursor = listing.nextCursor;
    if (!cursor) break;
  }
  if (cursor) throw new Error("REA tool discovery exceeded its page limit.");
  await call("open_binary", { path: selected.path, provider_id: "ghidra" }, "Opening the selected binary");
  opened = true;
  await call("binary_overview", {}, "Importing and analyzing with Ghidra", 270000);
  const procedures = await call("list_procedures", {}, "Reading functions");
  if (!Array.isArray(procedures)) throw new Error("REA returned an unexpected function inventory.");
  const wanted = selected.procedure;
  const procedure = wanted
    ? procedures.find((p) => p.value === wanted || p.address === wanted)
    : procedures.find((p) => p.value === "main") || procedures.find((p) => p.value === "entry") || procedures[0];
  if (wanted && !procedure) throw new Error(`Function ${wanted} was not found. Choose a name or address from the function inventory.`);
  if (procedure) await call("analyze_function", { procedure: procedure.address }, `Decompiling ${procedure.value || procedure.address}`);
  await call("list_strings", {}, "Reading strings and their locations");
} catch (error) {
  failure = String(error?.message || error);
} finally {
  if (opened) {
    try { await call("close_binary", {}, "Closing the analysis session", 10000); }
    catch (error) { failure = `${failure ? failure + "\n" : ""}Session cleanup: ${error.message}`; }
  }
  await client.close().catch(() => undefined);
}
// Persisted host-side evidence includes errors too; the runner owns the final manifest.
emit(failure ? { kind: "failed", error: failure } : { kind: "complete" });
if (failure) process.exitCode = 1;
