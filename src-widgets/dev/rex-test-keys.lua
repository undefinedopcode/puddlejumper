-- Test actions for PuddleJumper's Rex client: test-rexsession expects these
-- (as the server's --config; it edits and restores the file to test reloads,
-- so give it a copy), and they make keymaps and prompts easy to try by hand.
rex.action{name = 'hello', title = 'Hello', category = 'Test', run = function(ctx) return 'hi ' .. ctx.client_id end}
rex.action{name = 'ask', title = 'Ask', category = 'Test', run = function(ctx)
  local t, err = rex.ui.input{title = 'Your name?', value = 'x'}
  return t or err
end}
rex.action{name = 'choose', title = 'Choose', category = 'Test', run = function(ctx)
  local r, err = rex.ui.pick{title = 'Which?', items = {{id = 'a', title = 'A'}, {id = 'b', title = 'B'}}}
  return r or err
end}
rex.action{name = 'via_client', title = 'Via Client', category = 'Test', run = function(ctx)
  rex.client.queue('client.tab.new', {why = 'test'})
  return 'queued'
end}
rex.action{name = 'queue_then_fail', title = 'Queue Then Fail', category = 'Test', run = function(ctx)
  rex.client.queue('client.tab.new')
  error('after queueing')
end}
rex.bind('ctrl+shift+h', 'hello')
rex.bind('ctrl+a>c', 'hello')
rex.unbind('ctrl+shift+t')
