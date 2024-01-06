local AVFormatContext = {}
AVFormatContext.__index = AVFormatContext

function AVFormatContext.new(path)
  local self = setmetatable({}, AVFormatContext)
  local err, handle = avformat_open_input(path, nil, nil)
  if err < 0 then
    error("ffmpeg error: %d", err)
  end
  err = avformat_find_stream_info(handle, nil)
  if err < 0 then
    error("ffmpeg error: %d", err)
  end
  self.handle = handle
  return self
end

function AVFormatContext.__gc(self)
  avformat_close_input(self.handle)
end

local ReadThreadHandle = {}


return {
  AVFormatContext = AVFormatContext
}

