-- Stand-in for the real enrichment script. Replace with env/<your script>.lua.
local COLLECTOR_IP = os.getenv("COLLECTOR_IP") or "10.0.0.1"
local SITE = os.getenv("SITE") or "dc1"

function enrich(tag, ts, record)
    record["collector_ip"] = COLLECTOR_IP
    record["site"] = SITE
    record["pipeline"] = "edge"
    return 2, ts, record
end
