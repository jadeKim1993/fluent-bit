-- Measures the msgpack->Lua cost alone: the record is converted but not rebuilt.
function noop(tag, ts, record)
    return 0, ts, record
end
