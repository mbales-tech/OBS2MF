// OBS2MF - DirectShow capture of the "OBS Virtual Camera".
//
// OBS's virtual camera is a DirectShow-only softcam (invisible to MFEnumDeviceSources),
// so it is captured through a DirectShow graph:
//   OBS Virtual Camera -> Sample Grabber (NV12) -> Null Renderer
// All DirectShow / <initguid.h> usage is confined to ObsCapture.cpp (PImpl) so it does
// not collide with the Media Foundation GUID definitions in the rest of the broker.
#pragma once

#include <vector>

class ObsCapture
{
public:
    ObsCapture(unsigned dstW, unsigned dstH);
    ~ObsCapture();

    bool Start();                                  // build + run graph; true if connected
    void Stop();
    bool Running() const;
    bool GetFrame(std::vector<unsigned char>& out); // newest native NV12 -> dstW x dstH NV12
    unsigned long long FramesReceived() const;      // total BufferCB callbacks (diagnostic)

private:
    struct Impl;
    Impl* _impl;
};
