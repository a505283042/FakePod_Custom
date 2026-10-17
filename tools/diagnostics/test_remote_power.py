"""Execute the production BLE notification scheduler with host snapshots (not a firmware build)."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(text,signature):
 start=text.index(signature);left=text.index('{',start);depth=1;end=left+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 text=(ROOT/'src/system/ble_remote_service.cpp').read_text()
 body=function(text,'static void update_phone_state_notify(TickType_t now)')
 harness=r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
using TickType_t=uint32_t;
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
constexpr size_t kStatusPacketSize=20;
constexpr TickType_t kStatusResyncInterval=15000,kStatusEventPollInterval=250;
constexpr uint8_t kPhoneEventProtocolVersion=1;
uint16_t g_conn_handle=1,g_control_event_val_handle=1;
bool g_control_event_notify_enabled=true,g_force_status_notify=true,g_force_metadata_refresh=false;
TickType_t g_status_notify_due_tick=0,g_status_event_poll_due_tick=0,g_seek_resend_due=0,g_aux_check_due=0,g_remote_ui_resend_due=0,g_visual_resend_due=0,g_metadata_replay_due_tick=0;
uint8_t g_last_seek_status[19]={},g_last_remote_ui_status[6]={},g_last_visual_status[4]={},g_last_status_packet[20]={};
bool g_metadata_source_initialized=true;
uint8_t g_metadata_chunk_index=0,g_metadata_chunk_count=0,g_metadata_send_pass=0;
uint16_t g_metadata_sequence=1;
uint32_t g_last_status_playback_revision=0,g_last_status_seek_revision=0;
int seek_builds=0,status_builds=0,seek_packets=0,status_packets=0,metadata_packets=0;
uint8_t track=1;
bool tick_due(TickType_t now,TickType_t due){return int32_t(now-due)>=0;}
bool connection_is_encrypted(uint16_t){return true;}
bool music_remote_ui_available(){return true;}
void remote_music_seek_build_status(uint8_t*p,bool){seek_builds++;p[0]=5;p[3]=track;}
void build_remote_ui_status(uint8_t*p){p[0]=3;}
uint8_t player_home_remote_get_spectrum_style(){return 0;}
int notify_phone_event(uint16_t,const void*ptr,size_t){auto p=(const uint8_t*)ptr;if(p[0]==5)seek_packets++;if(p[0]==1)status_packets++;return 0;}
void build_status_packet(uint8_t*p,uint32_t*a,uint32_t*b){status_builds++;p[0]=1;p[6]=track;*a=track;*b=0;}
bool status_semantic_changed(const uint8_t*p){return p[6]!=g_last_status_packet[6];}
bool send_next_metadata_chunk(uint16_t){if(g_metadata_chunk_index<g_metadata_chunk_count){g_metadata_chunk_index++;metadata_packets++;return true;}return false;}
'''
 tests=r'''
int main(){
 // Drain initial snapshots; no change may cause a notification storm afterwards.
 for(uint32_t t=0;t<1000;t+=10)update_phone_state_notify(t);
 assert(seek_builds<=15);assert(seek_packets==1);assert(status_packets==1);
 int before=seek_builds;
 for(uint32_t t=1000;t<2000;t+=10)update_phone_state_notify(t);
 assert(seek_builds-before<=10);assert(seek_packets==1);
 track=2;for(uint32_t t=2000;t<2400;t+=10)update_phone_state_notify(t);
 assert(seek_packets==2);assert(status_packets==2);
 // Initial/changed revisions stay immediate; reliable replay remains bounded.
 for(uint32_t t=2400;t<16000;t+=10)update_phone_state_notify(t);
 assert(seek_packets==2);
 // Fast fragmentation bypasses the 100ms snapshot gate.
 g_metadata_chunk_count=8;g_metadata_chunk_index=0;
 for(uint32_t t=16000;t<16300;t+=10)update_phone_state_notify(t);
 assert(metadata_packets==8);
 int old=seek_builds;g_control_event_notify_enabled=false;
 for(uint32_t t=17000;t<18000;t+=10)update_phone_state_notify(t);
 assert(seek_builds==old);
 // Tick rollover must not permanently block future updates.
 g_control_event_notify_enabled=true;g_aux_check_due=0xfffffff0;
 update_phone_state_notify(0x20);assert(seek_builds==old+1);
 puts("PASS: BLE snapshot throttle, revision delivery, 15s refresh, fast metadata, unsubscribe and tick rollover");
}
'''
 with tempfile.TemporaryDirectory() as d:
  p=Path(d)/'test.cpp';p.write_text(harness+body+tests)
  subprocess.run(['g++','-std=c++17','-Wall','-Wextra',str(p),'-o',str(Path(d)/'test')],check=True)
  subprocess.run([str(Path(d)/'test')],check=True)
if __name__=='__main__':main()
