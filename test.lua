local os = require('os')
local math = require('math')

setDrawCallback(function ()
  glClearColor(0.5 + 0.5 * math.sin(50 * os.clock()), 0.0, 0.0, 1.0)
  glClear(GL_COLOR_BUFFER_BIT)
end)
