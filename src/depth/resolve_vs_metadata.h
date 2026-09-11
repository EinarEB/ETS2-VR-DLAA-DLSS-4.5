// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <map>
#include <array>
#include <vector>
#include <string>
#include <sstream>
#include <filesystem>
#include <fstream>

namespace ets2_resolve_vs {
using Microsoft::WRL::ComPtr;
class Metadata {
public:
    static constexpr size_t MaxEntries=1024,MaxCode=16384,MaxCacheBytes=8*1024*1024;
    static constexpr size_t MaxRecords=16,MaxSelected=8,MaxRecordBytes=128*1024,MaxSavedBytes=1024*1024;
    struct Entry {uint64_t generation=0;size_t size=0;std::string hash;std::vector<unsigned char> code;};
private:
    std::map<uint64_t,Entry> registry_;
    std::map<std::string,std::vector<unsigned char>> selected_;
    std::vector<std::string> records_;
    size_t cacheBytes_=0,recordBytes_=0,savedBytes_=0;
    uint64_t generation_=0,dropped_=0,saveErrors_=0;
    static bool HashValid(const std::string& value){
        if(value.size()!=64)return false;
        for(char c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        return true;
    }
    static uint64_t Address(const void* value){return reinterpret_cast<uint64_t>(value);}
    static void Buffer(std::ostream& out,ID3D11Buffer* buffer){
        D3D11_BUFFER_DESC desc{};if(buffer)buffer->GetDesc(&desc);
        out<<"{\"buffer\":"<<Address(buffer)<<",\"byte_width\":"<<desc.ByteWidth<<",\"usage\":"<<unsigned(desc.Usage)
           <<",\"bind_flags\":"<<desc.BindFlags<<",\"cpu_access\":"<<desc.CPUAccessFlags<<",\"misc_flags\":"<<desc.MiscFlags<<",\"structure_stride\":"<<desc.StructureByteStride<<'}';
    }
public:
    void Erase(uint64_t id)noexcept{auto it=registry_.find(id);if(it!=registry_.end()){cacheBytes_-=it->second.code.size();registry_.erase(it);}}
    void Register(uint64_t id,const void* data,size_t bytes,const std::string& hash)noexcept try{
        Erase(id);
        if(!id||!data||!bytes||bytes>1024*1024||!HashValid(hash)||registry_.size()>=MaxEntries){++dropped_;return;}
        Entry value;value.generation=++generation_;value.size=bytes;value.hash=hash;
#ifndef ETS2_DEPTH_EMBEDDED
        if(bytes<=MaxCode&&bytes<=MaxCacheBytes-cacheBytes_){const auto* p=static_cast<const unsigned char*>(data);value.code.assign(p,p+bytes);}
#endif
        const size_t added=value.code.size();registry_.emplace(id,std::move(value));cacheBytes_+=added;
    }catch(...){++dropped_;}
    const Entry* Lookup(uint64_t id)const{auto it=registry_.find(id);return it==registry_.end()?nullptr:&it->second;}
#ifndef ETS2_DEPTH_EMBEDDED
    bool Select(uint64_t id)noexcept try{
        const auto* entry=Lookup(id);if(!entry||entry->code.empty())return false;
        if(selected_.count(entry->hash))return true;
        if(selected_.size()>=MaxSelected)return false;
        selected_.emplace(entry->hash,entry->code);return true;
    }catch(...){++dropped_;return false;}
    size_t CacheBytes()const{return cacheBytes_;}
    size_t Entries()const{return registry_.size();}
    size_t Selected()const{return selected_.size();}
    const std::vector<unsigned char>* SelectedCode(const std::string& hash)const{
        auto it=selected_.find(hash);return it==selected_.end()?nullptr:&it->second;
    }
    // Metadata only. No Map, Copy, Draw, Dispatch, Flush, or resource creation.
    void Observe(ID3D11DeviceContext* context,uint64_t sequence,uint64_t source,uint64_t target,
                 unsigned sourceW,unsigned sourceH,unsigned targetW,unsigned targetH,
                 unsigned vertices,unsigned instances,unsigned firstVertex,unsigned firstInstance)noexcept try{
        if(!context||records_.size()>=MaxRecords||savedBytes_>=MaxSavedBytes)return;
        ComPtr<ID3D11VertexShader> shader;context->VSGetShader(&shader,nullptr,nullptr);
        const uint64_t id=Address(shader.Get());const auto* entry=Lookup(id);
        const bool selected=Select(id);
        ComPtr<ID3D11InputLayout> layout;context->IAGetInputLayout(&layout);
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
        ID3D11Buffer* rawVB[32]{};UINT strides[32]{},offsets[32]{};
        context->IAGetVertexBuffers(0,32,rawVB,strides,offsets);
        std::array<ComPtr<ID3D11Buffer>,32> vertex;
        for(unsigned i=0;i<32;++i)vertex[i].Attach(rawVB[i]);
        ID3D11Buffer* rawCB[14]{};UINT first[14]{},count[14]{};
        ComPtr<ID3D11DeviceContext1> context1;context->QueryInterface(IID_PPV_ARGS(&context1));
        if(context1)context1->VSGetConstantBuffers1(0,14,rawCB,first,count);else context->VSGetConstantBuffers(0,14,rawCB);
        std::array<ComPtr<ID3D11Buffer>,14> constants;
        for(unsigned i=0;i<14;++i)constants[i].Attach(rawCB[i]);
        ComPtr<ID3D11Buffer> index;DXGI_FORMAT indexFormat{};UINT indexOffset=0;context->IAGetIndexBuffer(&index,&indexFormat,&indexOffset);
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        std::ostringstream out;
        out<<"{\"sequence\":"<<sequence<<",\"observation\":\"before_pinned_resolve_depth_replay\",\"source_color\":"<<source<<",\"target_color\":"<<target
           <<",\"source_extent\":["<<sourceW<<','<<sourceH<<"],\"target_extent\":["<<targetW<<','<<targetH
           <<"],\"context\":"<<Address(context)<<",\"device\":"<<Address(device.Get())<<",\"thread\":"<<GetCurrentThreadId()
           <<",\"vs\":"<<id<<",\"vs_registered\":"<<(entry?"true":"false")
           <<",\"vs_generation\":"<<(entry?entry->generation:0)<<",\"vs_size\":"<<(entry?entry->size:0)
           <<",\"vs_sha256\":\""<<(entry?entry->hash:"")<<"\",\"vs_code_selected\":"<<(selected?"true":"false")
           <<",\"vertices\":"<<vertices<<",\"instances\":"<<instances<<",\"first_vertex\":"<<firstVertex<<",\"first_instance\":"<<firstInstance
           <<",\"ia_topology\":"<<unsigned(topology)<<",\"input_layout\":"<<Address(layout.Get())<<",\"vertex_buffers\":[";
        bool comma=false;
        for(unsigned i=0;i<32;++i)if(vertex[i]){if(comma)out<<',';comma=true;out<<"{\"slot\":"<<i<<",\"stride\":"<<strides[i]<<",\"offset\":"<<offsets[i]<<",\"resource\":";Buffer(out,vertex[i].Get());out<<'}';}
        out<<"],\"vs_constant_buffer_slices_available\":"<<(context1?"true":"false")<<",\"vs_constant_buffers\":[";comma=false;
        for(unsigned i=0;i<14;++i)if(constants[i]){if(comma)out<<',';comma=true;out<<"{\"slot\":"<<i<<",\"first_constant\":"<<first[i]<<",\"constant_count\":"<<count[i]<<",\"resource\":";Buffer(out,constants[i].Get());out<<'}';}
        out<<"],\"index_buffer\":{\"format\":"<<unsigned(indexFormat)<<",\"offset\":"<<indexOffset<<",\"resource\":";Buffer(out,index.Get());
        out<<"},\"buffer_contents_captured\":false,\"gpu_commands_issued\":0}";
        auto text=out.str();if(text.size()>MaxRecordBytes-recordBytes_){++dropped_;return;}
        records_.push_back(std::move(text));recordBytes_+=records_.back().size();
    }catch(...){++dropped_;}
    void Save(const std::wstring& directory,uint64_t epoch)noexcept try{
        if(records_.empty())return;
        const auto folder=std::filesystem::path(directory)/"resolve-vs";
        std::error_code error;std::filesystem::create_directories(folder,error);if(error){++saveErrors_;return;}
        for(const auto& [hash,code]:selected_){
            const auto file=folder/(hash+".dxbc");
            if(std::filesystem::exists(file))continue;
            std::ofstream output(file,std::ios::binary);output.write(reinterpret_cast<const char*>(code.data()),std::streamsize(code.size()));if(!output)++saveErrors_;
        }
        std::ostringstream row;
        row<<"{\"schema\":1,\"epoch\":"<<epoch<<",\"max_records\":"<<MaxRecords<<",\"max_record_bytes\":"<<MaxRecordBytes
              <<",\"max_selected_vs\":"<<MaxSelected<<",\"max_shader_bytes\":"<<MaxCode<<",\"cache_entries\":"<<Entries()
              <<",\"cache_bytes\":"<<cacheBytes_<<",\"dropped\":"<<dropped_<<",\"save_errors\":"<<saveErrors_<<",\"draws\":[";
        for(size_t i=0;i<records_.size();++i){if(i)row<<',';row<<records_[i];}
        row<<"]}\n";const auto text=row.str();
        if(text.size()<=MaxSavedBytes-savedBytes_){
            std::ofstream output(std::filesystem::path(directory)/"resolve-vs.jsonl",std::ios::app);
            output<<text;if(!output)++saveErrors_;else savedBytes_+=text.size();
        }else {savedBytes_=MaxSavedBytes;++dropped_;}
        records_.clear();recordBytes_=0;
    }catch(...){++saveErrors_;}
    // Records belong to one depth-route epoch; compiled shader cache survives.
    void ResetEpoch()noexcept{records_.clear();recordBytes_=0;}
#endif
};
}
