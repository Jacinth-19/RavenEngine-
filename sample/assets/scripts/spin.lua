-- RavenEngine sample script: spin the owning entity around Y.
-- Demonstrates: per-entity script instances + Raven.* bindings.
local speed = 45.0  -- degrees per second

function onStart(entity)
    Raven.log("spin.lua started on entity " .. tostring(entity))
end

function onUpdate(entity, dt)
    local rx, ry, rz = Raven.getRotation(entity)
    Raven.setRotation(entity, rx, ry + math.rad(speed) * dt, rz)
end
