local math = require('math')

setDrawCallback(function()
  glClearColor(0.5 + 0.5 * math.sin(5 * glfwGetTime()), 0.0, 0.0, 1.0)
  glClear(GL_COLOR_BUFFER_BIT)
end)


