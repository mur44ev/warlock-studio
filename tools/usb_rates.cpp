// Read actual Windows USB hub speed and active interrupt endpoint descriptors.
#include <windows.h>
#include <initguid.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <setupapi.h>
#include <iostream>
#include <vector>
int main(){
 HDEVINFO set=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
 std::cout<<"[";bool first=true;
 for(DWORD i=0;;++i){SP_DEVICE_INTERFACE_DATA item{};item.cbSize=sizeof(item);
  if(!SetupDiEnumDeviceInterfaces(set,nullptr,&GUID_DEVINTERFACE_USB_HUB,i,&item))break;
  DWORD needed=0;SetupDiGetDeviceInterfaceDetailW(set,&item,nullptr,0,&needed,nullptr);
  std::vector<unsigned char> detail(needed);auto path=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail.data());path->cbSize=sizeof(*path);
  if(!SetupDiGetDeviceInterfaceDetailW(set,&item,path,needed,nullptr,nullptr))continue;
  HANDLE hub=CreateFileW(path->DevicePath,GENERIC_WRITE,FILE_SHARE_WRITE|FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  if(hub==INVALID_HANDLE_VALUE)continue;
  USB_NODE_INFORMATION node{};node.NodeType=UsbHub;DWORD returned=0;
  if(DeviceIoControl(hub,IOCTL_USB_GET_NODE_INFORMATION,&node,sizeof(node),&node,sizeof(node),&returned,nullptr))
   for(unsigned port=1;port<=node.u.HubInformation.HubDescriptor.bNumberOfPorts;++port){
    std::vector<unsigned char> buf(4096);auto c=reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buf.data());c->ConnectionIndex=port;
    if(!DeviceIoControl(hub,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,c,(DWORD)buf.size(),c,(DWORD)buf.size(),&returned,nullptr))continue;
    if(c->ConnectionStatus!=DeviceConnected || c->DeviceDescriptor.idVendor!=0x3151)continue;
    if(!first)std::cout<<",";first=false;
    std::cout<<"{\"vid\":"<<c->DeviceDescriptor.idVendor<<",\"pid\":"<<c->DeviceDescriptor.idProduct<<",\"speed\":"<<unsigned(c->Speed)<<",\"endpoints\":[";
    for(unsigned n=0;n<c->NumberOfOpenPipes;++n){auto e=c->PipeList[n].EndpointDescriptor;
     if(n)std::cout<<",";std::cout<<"{\"address\":"<<unsigned(e.bEndpointAddress)<<",\"type\":"<<unsigned(e.bmAttributes&3)<<",\"interval\":"<<unsigned(e.bInterval)<<",\"packet\":"<<e.wMaxPacketSize<<"}";
    }std::cout<<"]}";
   }
  CloseHandle(hub);
 }SetupDiDestroyDeviceInfoList(set);std::cout<<"]\n";
}
