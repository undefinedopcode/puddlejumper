-- An example Rex configuration (~/.config/rex/init.lua) for PuddleJumper.
--
-- It uses only Rex's own Lua API, so it loads on Rex's server and on
-- atlantis alike, and the client actions every PuddleJumper has (pane.zoom,
-- client.sidebar.toggle, client.mode.enter, ...). Take what you like.
--
-- Everything is under one leader, ctrl+shift+a, then a key:
--
--   p  open a project          s  ssh to a host           f  focus mode
--   r  run a command in a split .  run it again           t  label the tab
--   l  follow a log beside      g  git status, floating    u  git pull
--   w  pane mode: h j k l move, v s split, z zoom, f float, x close, Esc out
--
-- The actions are in the command palette too, under their categories.
-- Things happen on the server's machine: folders, git, ssh and logs are
-- the server's.

local HOME = os.getenv('HOME') or ''
local TERMINAL = 'com.superlogical.terminal'

-- A terminal for a layout: options are cwd, command (a list), shell
-- ('none' runs command directly), initial_input, exit.
local function terminal(options)
  return rex.layout.block{flavor = TERMINAL .. '.shell', options = options or {}}
end

-- A command in a terminal that stays open when it ends, to read it.
local function command(dir, line)
  return terminal{cwd = dir, shell = 'none', command = {'sh', '-c', line}, exit = {on_completion = false}}
end

local function basename(path)
  return path:gsub('/+$', ''):match('[^/]+$') or path
end

local function trim(s)
  return (s:gsub('^%s+', ''):gsub('%s+$', ''))
end

-- The directory of the terminal the action came from (its shell reports
-- it as a file:// URL), else the home folder.
local function here(ctx)
  if ctx.block_id then
    local r = rex.block.call(TERMINAL, 'pwd', {})
    if r and r.pwd and r.pwd ~= '' then
      local path = r.pwd:gsub('^file://[^/]*', '')
      return (path:gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end))
    end
  end
  return HOME
end

-- The window holding the action's terminal, else the one showing.
local function window_of(ctx)
  local view = rex.session.view{}
  if not view then return nil end
  for _, w in ipairs(view.windows or {}) do
    if ctx.block_id and w.focused_block_id == ctx.block_id then return w.window_id end
  end
  return view.active_window_id
end

-- Call a session method; raise its error, so the action reports it.
local function must(result, err)
  if result == nil then error(err, 2) end
  return result
end

---------------------------------------------------------------- Projects --

-- A project's window: an editor, beside a shell over git status.
local function open_project(dir)
  must(rex.session.new_window{
    window_label = basename(dir),
    layout = rex.layout.horizontal(0.6,
      terminal{cwd = dir, initial_input = '${EDITOR:-vi} .\r'},
      rex.layout.vertical(0.5,
        terminal{cwd = dir},
        terminal{cwd = dir, initial_input = 'git status --short --branch; git log --oneline -8\r'})),
  })
  return basename(dir)
end

rex.action{
  name = 'open_project', title = 'Open Project…', category = 'Projects',
  description = 'Pick a folder in ~/projects (or give dir): it opens in a window of its own, an editor beside a shell and git status.',
  args = {type = 'object', properties = {dir = {type = 'string'}}},
  run = function(ctx, args)
    if args.dir then return open_project(args.dir) end
    local r, err = rex.exec({'find', HOME .. '/projects', '-mindepth', '1', '-maxdepth', '1', '-type', 'd', '-not', '-name', '.*'})
    if not r or not r.ok then error('listing ~/projects: ' .. (err or r.stderr)) end
    local items = {}
    for path in r.stdout:gmatch('[^\n]+') do
      items[#items + 1] = {id = path, title = basename(path), subtitle = path}
    end
    if #items == 0 then error('~/projects is empty') end
    table.sort(items, function(a, b) return a.title:lower() < b.title:lower() end)
    local dir = rex.ui.pick{title = 'Open Project', items = items}
    if not dir then return nil end -- cancelled
    return open_project(dir)
  end,
}

----------------------------------------------------------------- Layouts --

rex.action{
  name = 'follow_log', title = 'Follow a Log Beside This…', category = 'Layouts',
  description = 'A terminal following a file (tail -F) or a systemd unit (journalctl -fu), under this one. ratio: what this one keeps.',
  args = {type = 'object', properties = {
    file = {type = 'string'}, unit = {type = 'string'}, ratio = {type = 'number', minimum = 0.1, maximum = 0.9}}},
  run = function(ctx, args)
    local cmd
    if args.unit then
      cmd = {'journalctl', '-f', '-u', args.unit}
    else
      local file = args.file or rex.ui.input{title = 'Follow which file?', placeholder = '/var/log/…'}
      if not file or file == '' then return nil end
      cmd = {'tail', '-F', file}
    end
    must(rex.session.new_split{
      anchor_id = ctx.block_id, direction = 'vertical', side = 'after', ratio = args.ratio or 0.65, focus = false,
      layout = terminal{cwd = here(ctx), shell = 'none', command = cmd, exit = {on_completion = false}},
    })
  end,
}

-- The last command run in a split, for "run it again" (kept until the
-- configuration is reloaded).
local last_run

local function run_in_split(ctx, line, dir)
  last_run = {line = line, dir = dir}
  must(rex.session.new_split{
    anchor_id = ctx.block_id, direction = 'horizontal', side = 'after', ratio = 0.55,
    layout = command(dir, line),
  })
  return line
end

rex.action{
  name = 'run_in_split', title = 'Run in a Split…', category = 'Layouts',
  description = 'Asks for a command and runs it beside this terminal, in its directory; the split stays open when it ends.',
  args = {type = 'object', properties = {command = {type = 'string'}}},
  run = function(ctx, args)
    local line = args.command or rex.ui.input{title = 'Run', placeholder = 'make test',
                                              value = last_run and last_run.line or ''}
    if not line or trim(line) == '' then return nil end
    return run_in_split(ctx, line, here(ctx))
  end,
}

rex.action{
  name = 'run_again', title = 'Run It Again', category = 'Layouts',
  description = 'Runs the last Run in a Split command again, where it ran.',
  run = function(ctx)
    if not last_run then error('nothing has been run in a split yet') end
    return run_in_split(ctx, last_run.line, last_run.dir)
  end,
}

--------------------------------------------------------------------- Git --

rex.action{
  name = 'git_status', title = 'Git Status', category = 'Git',
  description = 'Status and recent history of the repository here, in a floating terminal (Enter closes it).',
  run = function(ctx)
    local dir = here(ctx)
    must(rex.session.new_layer{
      window_id = window_of(ctx), bounds = {0.15, 0.1, 0.7, 0.75}, focus = true,
      layout = terminal{cwd = dir, shell = 'none', command = {'sh', '-c',
        'git status && echo && git log --oneline --graph --decorate -20; printf "\\n(Enter closes)"; read _'}},
    })
  end,
}

rex.action{
  name = 'git_pull', title = 'Git Pull', category = 'Git',
  description = 'git pull --ff-only in the repository here; says how it went.',
  run = function(ctx)
    local r, err = rex.exec({'git', 'pull', '--ff-only'}, {cwd = here(ctx), timeout = 120})
    if not r then error(err) end
    local said = trim(r.ok and r.stdout or r.stderr)
    if not r.ok then error(said ~= '' and said or ('git pull failed (' .. r.code .. ')')) end
    return said:match('[^\n]*$') -- the last line: "Already up to date." or the summary
  end,
}

rex.action{
  name = 'label_tab', title = 'Label Tab from Folder and Branch', category = 'Git',
  description = "Names this window after the terminal's folder, and its git branch if it has one.",
  run = function(ctx)
    local dir = here(ctx)
    local label = basename(dir)
    local r = rex.exec({'git', 'rev-parse', '--abbrev-ref', 'HEAD'}, {cwd = dir, stderr = false})
    if r and r.ok then label = label .. ' · ' .. trim(r.stdout) end
    must(rex.session.set_window_label{window_id = window_of(ctx), label = label})
    return label
  end,
}

--------------------------------------------------------------------- SSH --

-- The hosts ~/.ssh/config names (patterns left out).
local function ssh_hosts()
  local hosts, seen = {}, {}
  local f = io.open(HOME .. '/.ssh/config')
  if not f then return hosts end
  for line in f:lines() do
    local names = line:match('^%s*[Hh][Oo][Ss][Tt]%s+(.+)$')
    for name in (names or ''):gmatch('%S+') do
      if not name:find('[%*%?!]') and not seen[name] then
        seen[name] = true
        hosts[#hosts + 1] = {id = name, title = name}
      end
    end
  end
  f:close()
  return hosts
end

rex.action{
  name = 'ssh_to', title = 'SSH to a Host…', category = 'Hosts',
  description = 'Pick a host from ~/.ssh/config; ssh to it in a window of its own, named after it.',
  args = {type = 'object', properties = {host = {type = 'string'}}},
  run = function(ctx, args)
    local host = args.host
    if not host then
      local hosts = ssh_hosts()
      if #hosts == 0 then error('no hosts in ~/.ssh/config') end
      host = rex.ui.pick{title = 'SSH to', items = hosts, allow_other = true}
      if type(host) == 'table' then host = host.text end -- typed, not picked
      if not host or host == '' then return nil end
    end
    must(rex.session.new_window{window_label = host, layout = terminal{shell = 'none', command = {'ssh', host},
                                                                      exit = {on_completion = false}}})
    return host
  end,
}

-------------------------------------------------------------------- View --

local focused = false

rex.action{
  name = 'focus_mode', title = 'Focus Mode', category = 'View',
  description = 'This terminal fills the window, the sidebar goes and the text grows; again puts it all back.',
  run = function(ctx)
    focused = not focused
    -- Queued: the app performs these once the action returns.
    rex.client.queue('pane.zoom')
    rex.client.queue('client.sidebar.toggle')
    rex.client.queue(focused and 'client.font.increase' or 'client.font.reset')
    return focused and 'Focus on' or 'Focus off'
  end,
}

-- Pane mode: keys move between and rearrange terminals until Esc.
rex.mode('pane', {exclusive = true})

rex.action{
  name = 'focus_pane', title = 'Focus the Terminal Beside', category = 'View',
  args = {type = 'object', required = {'direction'},
          properties = {direction = {type = 'string', enum = {'left', 'right', 'up', 'down'}}}},
  run = function(ctx, args)
    must(rex.session.focus_direction{direction = args.direction})
  end,
}

----------------------------------------------------------------- Keys --

local leader = 'ctrl+shift+a>'
rex.bind(leader .. 'p', 'open_project')
rex.bind(leader .. 'l', 'follow_log')
rex.bind(leader .. 'r', 'run_in_split')
rex.bind(leader .. '.', 'run_again')
rex.bind(leader .. 'g', 'git_status')
rex.bind(leader .. 'u', 'git_pull')
rex.bind(leader .. 't', 'label_tab')
rex.bind(leader .. 's', 'ssh_to')
rex.bind(leader .. 'f', 'focus_mode')
rex.bind(leader .. 'w', 'client.mode.enter', {mode = 'pane'})

for key, direction in pairs{h = 'left', j = 'down', k = 'up', l = 'right'} do
  rex.bind('pane/' .. key, 'focus_pane', {direction = direction})
end
rex.bind('pane/v', 'pane.split.right')
rex.bind('pane/s', 'pane.split.down')
rex.bind('pane/z', 'pane.zoom')
rex.bind('pane/f', 'pane.float')
rex.bind('pane/x', 'pane.close')
rex.bind('pane/escape', 'client.mode.exit')
