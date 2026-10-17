"""Differential regression for actual NAS folder enumerators/counting helpers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, name):
    start = text.index(name)
    start = text.rfind("\n", 0, start) + 1
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    text = (ROOT / "src/media/library/nas_library_source.cpp").read_text()
    names = ["path_has_prefix(", "extract_level1(", "same_component(", "copy_component_path(",
             "level1_option_at(", "level2_option_at(", "size_t nas_library_source_folder_option_count("]
    helpers = "\n".join(function(text, name) for name in names)
    before = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
enum class PlayerFolderScope { All, Level1, Level2 };
struct Track { uint32_t path_off; };
static struct { uint32_t track_count = 0; std::vector<Track> tracks; } g_catalog;
static std::vector<std::string> paths;
static uint64_t lookups = 0;
static bool ready = true;
bool nas_library_source_ready() { return ready; }
const char *pool_str(uint32_t offset) { ++lookups;return paths.at(offset).c_str(); }
void publish() {
 std::sort(paths.begin(),paths.end());
 g_catalog.track_count = paths.size();g_catalog.tracks.clear();
 for(uint32_t i=0;i<paths.size();++i)g_catalog.tracks.push_back({i});
}
'''
    after = r'''
uint64_t legacy_count(PlayerFolderScope scope,const char *parent) {
 uint64_t count=0;char path[384] = {};
 if(scope == PlayerFolderScope::Level1) {
  while(level1_option_at(count,false,path,sizeof(path),nullptr))++count;
 } else if(scope == PlayerFolderScope::Level2) {
  while(level2_option_at(parent,count,path,sizeof(path),nullptr))++count;
 }
 return count;
}
void compare() {
 assert(nas_library_source_folder_option_count(PlayerFolderScope::Level1,nullptr) == legacy_count(PlayerFolderScope::Level1,nullptr));
 for(auto parent : {"/A/","/a/","/中文/","/missing/","/","",static_cast<const char*>(nullptr)}) {
  assert(nas_library_source_folder_option_count(PlayerFolderScope::Level2,parent) == legacy_count(PlayerFolderScope::Level2,parent));
 }
 assert(nas_library_source_folder_option_count(PlayerFolderScope::All,nullptr) == 0);
}
int main() {
 compare();
 paths={"/root.flac","/A/direct.flac","/A/B/one.flac","/A/B/C/deep.flac","/A/C/one.flac", "/A/C/two.flac", "/a/B/one.flac","/中文/专辑/歌曲.flac","invalid"};
 publish();compare();
 assert(nas_library_source_folder_option_count(PlayerFolderScope::Level1,nullptr) == 3);
 assert(nas_library_source_folder_option_count(PlayerFolderScope::Level2,"/A/") == 2);
 char path[384] = {};uint32_t count=0;
 assert(level2_option_at("/A/",0,path,sizeof(path),&count) && std::string(path) == "/A/B/" && count == 1);
 std::mt19937 random(1464);
 for(unsigned round=0;round<50;++round) {
  paths.clear();
  for(unsigned i=0;i<200;++i) {
   auto folder=std::string("/") + (random()%2 ? "A" : "中文") + "/";
   auto child=random()%8;
   paths.push_back(folder+(child ? std::to_string(child)+"/" : "")+std::to_string(i)+".flac");
  }
  publish();compare();
 }
 paths.clear();
 for(unsigned d=0;d<200;++d)for(unsigned n=0;n<10;++n)paths.push_back("/D"+std::to_string(d)+"/"+std::to_string(n)+".flac");
 publish();lookups=0;assert(legacy_count(PlayerFolderScope::Level1,nullptr) == 200);auto old=lookups;
 lookups=0;assert(nas_library_source_folder_option_count(PlayerFolderScope::Level1,nullptr) == 200);auto fast=lookups;
 assert(fast < old/50);
 ready=false;assert(nas_library_source_folder_option_count(PlayerFolderScope::Level1,nullptr) == 0);
 std::cout << "PASS: empty/Unicode/nested/case-sensitive/random folder equivalence; fixture path lookups " << old << " -> " << fast << " (not device timing)\n";
}
'''
    with tempfile.TemporaryDirectory() as folder:
        source, binary = Path(folder) / "test.cpp", Path(folder) / "test"
        source.write_text(before + helpers + after)
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
