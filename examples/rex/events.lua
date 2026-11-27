-- Companion to init.lua: things done when something happens. rex.on works
-- only in a script, so run this one beside the app and leave it running:
--
--   rex do ~/.config/rex/events.lua --all -C PuddleJumper
--
-- (or atlantis do, the same way).
--
-- --all hears every session; -C names the app that shows notifications.
--
--   * A window takes the name of its terminal's folder (and git branch) as
--     the shell changes directory, unless it was named some other way.
--   * A command run in a terminal of its own (Run in a Split, Follow a Log)
--     that fails raises a desktop notification.

local named = {} -- window_id -> the label this script gave it
local first = {} -- window_id -> its label when this script first saw it

local function basename(path)
  return path:gsub('/+$', ''):match('[^/]+$') or path
end

local function path_of(url)
  local path = url:gsub('^file://[^/]*', '')
  return (path:gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end))
end

rex.on('terminal.pwd_changed', function(ctx, ev)
  if not ev.pwd or ev.pwd == '' then return end
  local view = rex.session.view{}
  if not view then return end
  for _, w in ipairs(view.windows or {}) do
    -- Only the window's focused terminal names it, and never over a name
    -- someone gave it since (its first one is the server's default).
    first[w.window_id] = first[w.window_id] or w.label
    if w.focused_block_id == ctx.block_id and (w.label == first[w.window_id] or w.label == named[w.window_id]) then
      local dir = path_of(ev.pwd)
      local label = basename(dir)
      local r = rex.exec({'git', 'rev-parse', '--abbrev-ref', 'HEAD'}, {cwd = dir, stderr = false})
      if r and r.ok then label = label .. ' · ' .. r.stdout:gsub('%s+$', '') end
      if label ~= w.label then
        named[w.window_id] = label
        rex.session.set_window_label{window_id = w.window_id, label = label}
      end
    end
  end
end)

rex.on('terminal.child_exited', function(ctx, ev)
  if (ev.exit_code or 0) == 0 then return end
  local block = rex.session.describe_block{block_id = ctx.block_id}
  local label = block and block.block and block.block.label or ''
  rex.client.queue('client.notify', {
    title = 'A command failed (' .. tostring(ev.exit_code) .. ')',
    body = (label ~= '' and label or ctx.block_id) .. (ev.error and (': ' .. ev.error) or ''),
  })
end)
