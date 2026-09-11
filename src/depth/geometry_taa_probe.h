// SPDX-License-Identifier: MIT
#pragma once
// Private observer only. The host serializes these hooks with its route mutex.
// No draw, state replacement, Flush, wait, or draw-callback file I/O occurs here.
// The host supplies registry hashes for the shaders actually bound at each hook.
#include <d3d11_4.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

namespace ets2_geometry_taa {
using Microsoft::WRL::ComPtr;
inline uint64_t FirstEpoch=600;
inline constexpr uint64_t EpochCount=16;
inline constexpr size_t MaxBundles=256, MaxSnapshots=16, MaxDepths=64;
inline constexpr unsigned MaxZeroMinimumRangesPerVS=2,MaxRaisedMinimumRangesPerVS=1;
inline constexpr size_t MaxGeometryPerSnapshot=2*(MaxZeroMinimumRangesPerVS+MaxRaisedMinimumRangesPerVS);
inline constexpr size_t MaxRetainedGeometryPerSnapshot=32;
inline constexpr uint64_t DiscoveryEpochs=2;
inline constexpr size_t DiscoveryBundles=128,DiscoveryCodeBytes=2*1024*1024;
inline constexpr uint32_t DiscoveryVertexBytes=48*1024,DiscoveryIndexBytes=4*1024;
inline constexpr const char* WorldCandidate="3e1b8172df95833c5a995e8674b67aba8644987dffe8c6ffda2125eb3f58535e";
inline constexpr const char* CabCandidate="ff4b90b312ce4180d859fc40c01d59b3eaad77b594001121fde0843849fa0513";
struct DrawArgs {
    bool indexed=true;
    uint32_t count=0,instances=0,first=0;
    int32_t baseVertex=0;
    uint32_t firstInstance=0;
};
struct PositionLayout {bool valid=false;uint32_t format=0,slot=0,offset=0,step=0;};
inline constexpr uint32_t MaxVertexBytes=128*1024,MaxIndexBytes=256;
inline bool ByteWindow(uint32_t total,uint64_t start,uint32_t requested,uint32_t& bytes)noexcept{
    if(start>=total||start>UINT32_MAX||!requested)return false;
    bytes=uint32_t((std::min)(uint64_t(requested),uint64_t(total)-start));return bytes>0;
}
inline bool InEpochWindow(uint64_t epoch)noexcept{return epoch>=FirstEpoch&&epoch-FirstEpoch<EpochCount;}
inline bool Slice(uint32_t bytes,uint32_t first,uint32_t count,uint32_t row,uint32_t rows,uint32_t& offset)noexcept{
    const uint64_t endRow=uint64_t(row)+rows,begin=(uint64_t(first)+row)*16,end=begin+uint64_t(rows)*16;
    if(!rows||endRow>count||end>bytes||begin>UINT32_MAX)return false;
    offset=uint32_t(begin);return true;
}
// Geometry is restricted to a complete, non-array, single-sample mip-zero DSV.
// Compare meaningful descriptor fields rather than union/padding bytes.
inline bool SameView(const D3D11_DEPTH_STENCIL_VIEW_DESC& a,const D3D11_DEPTH_STENCIL_VIEW_DESC& b)noexcept{
    return a.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D&&b.ViewDimension==a.ViewDimension&&
        a.Format==b.Format&&a.Flags==b.Flags&&a.Texture2D.MipSlice==0&&b.Texture2D.MipSlice==0;
}
struct GeometryKey {
    uint64_t epoch=0,depth=0,clearGeneration=0;
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    std::string vsHash;
    uint32_t minDepthBits=0,maxDepthBits=0;
};
enum class DepthRange { unsupported,minimum_zero,minimum_positive };
inline uint32_t FloatBits(float value)noexcept{uint32_t bits=0;std::memcpy(&bits,&value,4);return bits;}
inline DepthRange ClassifyDepthRange(uint32_t minimumBits,uint32_t maximumBits)noexcept{
    float minimum=0,maximum=0;std::memcpy(&minimum,&minimumBits,4);std::memcpy(&maximum,&maximumBits,4);
    if(!std::isfinite(minimum)||!std::isfinite(maximum)||minimum<0||maximum>1||maximum<=minimum)return DepthRange::unsupported;
    return minimum==0?DepthRange::minimum_zero:DepthRange::minimum_positive;
}
inline bool RangeBudgetAvailable(DepthRange range,unsigned zeroCount,unsigned raisedCount)noexcept{
    return range==DepthRange::minimum_zero?zeroCount<MaxZeroMinimumRangesPerVS:
        range==DepthRange::minimum_positive&&raisedCount<MaxRaisedMinimumRangesPerVS;
}
inline bool MatchesSnapshot(const GeometryKey& key,uint64_t epoch,uint64_t depth,uint64_t clearGeneration,
                            const D3D11_DEPTH_STENCIL_VIEW_DESC& view)noexcept{
    return key.epoch==epoch&&key.depth==depth&&key.clearGeneration==clearGeneration&&SameView(key.view,view);
}
inline bool SameGeometrySource(const GeometryKey& a,const GeometryKey& b)noexcept{
    return a.vsHash==b.vsHash&&MatchesSnapshot(a,b.epoch,b.depth,b.clearGeneration,b.view);
}
inline bool SameGeometryKey(const GeometryKey& a,const GeometryKey& b)noexcept{
    return SameGeometrySource(a,b)&&a.minDepthBits==b.minDepthBits&&a.maxDepthBits==b.maxDepthBits;
}

class Probe {
    struct ContextLock {
        ComPtr<ID3D11Multithread> protection;
        bool held=false;
        explicit ContextLock(ID3D11DeviceContext* c){
            if(c&&SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&protection)))&&protection->GetMultithreadProtected()){
                protection->Enter();held=true;
            }
        }
        ~ContextLock(){if(held)protection->Leave();}
    };
    struct BufferSlice {
        ComPtr<ID3D11Buffer> source;
        uint32_t first=0,count=0,byteWidth=0,offset=0,bytes=0,row=0;
    };
    struct Sample {
        uint64_t id=0,epoch=0,sequence=0,vsGeneration=0;
        bool taa=false,submitted=false,retired=false,done=false,written=false,valid=false;
        unsigned snapshot=0;
        GeometryKey key;
        std::string vsHash,psHash,metadata="{}";
        const char* reason="not-submitted";
        HRESULT hr=S_OK;
        BufferSlice slice[2];
        ComPtr<ID3D11Buffer> staging;
        ComPtr<ID3D11Query> query;
        // Hold IA identities for the bounded session; arena addresses cannot be
        // mistaken for a new allocation while records are being compared.
        std::array<ComPtr<ID3D11Buffer>,33> ia;
        unsigned bytes=0;
        std::array<uint32_t,32> words{};
        DrawArgs args{};PositionLayout position{};
        ComPtr<ID3D11Buffer> iaStaging;
        uint32_t vertexOffset=0,vertexBytes=0,indexOffset=0,indexBytes=0,vertexStride=0,bindingOffset=0,indexFormat=0;
        bool iaSubmitted=false,iaValid=false;
        const char* iaReason="not-requested";
        std::vector<unsigned char> iaData;
    };
    struct Depth {
        ComPtr<ID3D11Resource> resource;
        uint64_t generation=0;
    };
    struct SnapshotRecord {
        bool present=false,ambiguous=false,written=false,referenced=false;
        uint64_t epoch=0,depth=0,clearGeneration=0;
        ComPtr<ID3D11Texture2D> color;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        unsigned width=0,height=0;
        std::array<uint64_t,MaxRetainedGeometryPerSnapshot> geometry{};
        unsigned geometryCount=0,taaCount=0,copyCount=0;
        uint64_t taa=0;
    };
    struct PairRecord {
        bool present=false,ambiguous=false,written=false;
        uint64_t epoch=0,runtimeGeneration=0;
        unsigned source[2]{};
        SnapshotRecord eye[2];
    };
    struct State {
        ComPtr<ID3D11DeviceContext> context;
        std::array<Sample,MaxBundles> samples;
        std::array<Depth,MaxDepths> depths;
        std::array<std::array<SnapshotRecord,MaxSnapshots>,EpochCount> snapshots;
        std::array<PairRecord,EpochCount> pairs;
        std::array<bool,EpochCount> seen{};
        size_t used=0,depthCount=0,writtenBytes=0;
        uint64_t epoch=UINT64_MAX,startTick=0;
        unsigned rejected=0,rangeRejected=0,overflows=0;
        bool checked=false,armed=false,terminal=false,failed=false,regressed=false;
        HANDLE log=INVALID_HANDLE_VALUE;
        std::wstring directory;
        std::map<uint64_t,PositionLayout> layouts;
        std::map<std::string,std::vector<unsigned char>> shaderCode;
        size_t shaderCodeBytes=0;
        bool modeChecked=false,discovery=false;
    };
    std::unique_ptr<State> state_{new(std::nothrow) State};
    static constexpr size_t MaxLogBytes=4*1024*1024;
    static constexpr uint64_t TimeoutMs=30000;
    static uint64_t Address(const void* p)noexcept{return reinterpret_cast<uint64_t>(p);}
    static void Stream(std::ostringstream& out){
        out.exceptions(std::ios::badbit|std::ios::failbit);out.imbue(std::locale::classic());
        out<<std::scientific<<std::setprecision(std::numeric_limits<float>::max_digits10);
    }
    static void Quote(std::ostream& out,const std::string& text){
        out<<'"';for(unsigned char c:text){
            if(c=='"'||c=='\\')out<<'\\'<<char(c);
            else if(c<32)out<<"\\u00"<<"0123456789abcdef"[c>>4]<<"0123456789abcdef"[c&15];
            else out<<char(c);
        }out<<'"';
    }
    template<class T>static void Raw(std::ostream& out,const T& value){
        static_assert(sizeof(T)%4==0);std::array<uint32_t,sizeof(T)/4> words{};
        std::memcpy(words.data(),&value,sizeof(T));out<<'[';
        for(size_t i=0;i<words.size();++i){if(i)out<<',';out<<words[i];}out<<']';
    }
    static std::string Hash(const void* bytes,unsigned count){
        BCRYPT_ALG_HANDLE algorithm=nullptr;unsigned char hash[32]{};
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
        const auto status=BCryptHash(algorithm,nullptr,0,(PUCHAR)bytes,count,hash,sizeof(hash));
        BCryptCloseAlgorithmProvider(algorithm,0);if(status<0)return {};
        std::string text;text.reserve(64);for(auto b:hash){text+="0123456789abcdef"[b>>4];text+="0123456789abcdef"[b&15];}return text;
    }
    bool Active()const noexcept{return state_&&state_->armed&&!state_->terminal&&!state_->regressed&&InEpochWindow(state_->epoch)&&
        (!state_->discovery||state_->epoch-FirstEpoch<DiscoveryEpochs);}
    void Mode()noexcept{if(state_&&!state_->modeChecked){wchar_t flag[2]{};state_->modeChecked=true;
        state_->discovery=GetEnvironmentVariableW(L"ETS2_GEOMETRY_DEPTH_DISCOVERY",flag,2)==1&&flag[0]==L'1';}}
    bool Pending()const noexcept{
        if(state_)for(size_t i=0;i<state_->used;++i)if(state_->samples[i].submitted&&!state_->samples[i].retired)return true;
        return false;
    }
    void Failure()noexcept{if(state_)state_->failed=true;}
    void Reject()noexcept{if(state_)++state_->rejected;}
    bool Context(ID3D11DeviceContext* c)noexcept{
        if(!c||c!=state_->context.Get()||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){Failure();Reject();return false;}return true;
    }
    Depth* FindDepth(ID3D11Resource* resource,bool insert){
        for(size_t i=0;i<state_->depthCount;++i)if(state_->depths[i].resource.Get()==resource)return &state_->depths[i];
        if(!insert||!resource)return nullptr;
        if(state_->depthCount==MaxDepths){++state_->overflows;Failure();return nullptr;}
        auto& d=state_->depths[state_->depthCount++];d.resource=resource;return &d;
    }
    Sample* Allocate(bool taa,uint64_t sequence){
        if(state_->used==MaxBundles){++state_->overflows;Failure();return nullptr;}
        auto& s=state_->samples[state_->used++];s.id=state_->used;s.epoch=state_->epoch;s.sequence=sequence;s.taa=taa;return &s;
    }
    static void SampleFailure(Sample& s,const char* reason,HRESULT hr=E_FAIL)noexcept{s.reason=reason;s.hr=hr;s.done=true;s.valid=false;}
    static bool GetSlice(ID3D11DeviceContext1* c,bool pixel,unsigned row,unsigned rows,BufferSlice& out){
        if(pixel)c->PSGetConstantBuffers1(0,1,&out.source,&out.first,&out.count);
        else c->VSGetConstantBuffers1(0,1,&out.source,&out.first,&out.count);
        D3D11_BUFFER_DESC d{};if(out.source)out.source->GetDesc(&d);out.byteWidth=d.ByteWidth;out.row=row;out.bytes=rows*16;
        return out.source&&(d.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&Slice(d.ByteWidth,out.first,out.count,row,rows,out.offset);
    }
    void Queue(ID3D11DeviceContext* c,Sample& s){
        ComPtr<ID3D11DeviceContext1> c1;
        if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1)))){SampleFailure(s,"context1-unavailable");return;}
        if(!GetSlice(c1.Get(),s.taa,s.taa?8u:0u,s.taa?1u:8u,s.slice[0])||
           (s.taa&&!GetSlice(c1.Get(),false,0,3,s.slice[1]))){SampleFailure(s,"bound-cb0-slice-invalid");return;}
        s.bytes=s.slice[0].bytes+(s.taa?s.slice[1].bytes:0);
        ComPtr<ID3D11Device> device;c->GetDevice(&device);
        D3D11_BUFFER_DESC b{};b.ByteWidth=s.bytes;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HRESULT hr=device->CreateBuffer(&b,nullptr,&s.staging);if(FAILED(hr)){SampleFailure(s,"staging-create-failed",hr);return;}
        const D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};hr=device->CreateQuery(&q,&s.query);
        if(FAILED(hr)){SampleFailure(s,"query-create-failed",hr);return;}
        s.submitted=true;unsigned destination=0;
        for(unsigned i=0;i<(s.taa?2u:1u);++i){const auto& slice=s.slice[i];const D3D11_BOX box{slice.offset,0,0,slice.offset+slice.bytes,1,1};
            c->CopySubresourceRegion(s.staging.Get(),0,destination,0,0,slice.source.Get(),0,&box);destination+=slice.bytes;}
        if(!s.taa)QueueIA(c,s);
        c->End(s.query.Get());s.reason="pending";
    }
    void QueueIA(ID3D11DeviceContext* c,Sample& s){
        ComPtr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);
        const auto found=state_->layouts.find(Address(layout.Get()));
        if(found==state_->layouts.end()||!found->second.valid){s.iaReason="position-layout-unregistered";return;}
        s.position=found->second;
        if(s.position.slot>=32||s.position.step||s.position.offset==UINT32_MAX){s.iaReason="position-layout-unsupported";return;}
        ComPtr<ID3D11Buffer> vertex,index;UINT stride=0,offset=0,ioffset=0;DXGI_FORMAT iformat{};
        c->IAGetVertexBuffers(s.position.slot,1,&vertex,&stride,&offset);c->IAGetIndexBuffer(&index,&iformat,&ioffset);
        const unsigned indexSize=iformat==DXGI_FORMAT_R16_UINT?2:iformat==DXGI_FORMAT_R32_UINT?4:0;
        if(!vertex||!index||!stride||!indexSize){s.iaReason="ia-binding-unsupported";return;}
        D3D11_BUFFER_DESC vb{},ib{};vertex->GetDesc(&vb);index->GetDesc(&ib);
        const uint64_t vertexStart=uint64_t(offset)+uint64_t((std::max)(s.args.baseVertex,0))*stride;
        const uint64_t indexStart=uint64_t(ioffset)+uint64_t(s.args.first)*indexSize;
        const uint32_t maxVertex=state_->discovery?DiscoveryVertexBytes:MaxVertexBytes,maxIndex=state_->discovery?DiscoveryIndexBytes:MaxIndexBytes;
        if(!ByteWindow(vb.ByteWidth,vertexStart,maxVertex,s.vertexBytes)||
           !ByteWindow(ib.ByteWidth,indexStart,(std::min)(maxIndex,s.args.count>(maxIndex/indexSize)?maxIndex:s.args.count*indexSize),s.indexBytes)){
            s.iaReason="ia-byte-window-invalid";return;
        }
        s.vertexOffset=uint32_t(vertexStart);s.indexOffset=uint32_t(indexStart);s.vertexStride=stride;s.bindingOffset=offset;s.indexFormat=unsigned(iformat);
        ComPtr<ID3D11Device> device;c->GetDevice(&device);
        D3D11_BUFFER_DESC staging{};staging.ByteWidth=s.vertexBytes+s.indexBytes;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&staging,nullptr,&s.iaStaging))){s.iaReason="ia-staging-create-failed";return;}
        const D3D11_BOX vbox{s.vertexOffset,0,0,s.vertexOffset+s.vertexBytes,1,1};
        const D3D11_BOX ibox{s.indexOffset,0,0,s.indexOffset+s.indexBytes,1,1};
        c->CopySubresourceRegion(s.iaStaging.Get(),0,0,0,0,vertex.Get(),0,&vbox);
        c->CopySubresourceRegion(s.iaStaging.Get(),0,s.vertexBytes,0,0,index.Get(),0,&ibox);
        s.iaSubmitted=true;s.iaReason="pending";
    }
    void WriteIA(std::ostream& out,Sample& s){
        std::string file;
        if(s.iaValid){file="ia-"+std::to_string(s.id)+".bin";std::wstring path=state_->directory+std::wstring(file.begin(),file.end());
            HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            DWORD wrote=0;const bool valid=h!=INVALID_HANDLE_VALUE&&WriteFile(h,s.iaData.data(),DWORD(s.iaData.size()),&wrote,nullptr)&&wrote==s.iaData.size();
            if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);if(!valid){s.iaValid=false;s.iaReason="ia-file-write-failed";Failure();}}
        out<<",\"ia_capture\":{\"valid\":"<<(s.iaValid?"true":"false")<<",\"reason\":\""<<s.iaReason<<"\",\"file\":\""<<file
           <<"\",\"sha256\":\""<<(s.iaValid?Hash(s.iaData.data(),unsigned(s.iaData.size())):"")<<"\",\"vertex_byte_offset\":"<<s.vertexOffset
           <<",\"vertex_bytes\":"<<s.vertexBytes<<",\"index_byte_offset\":"<<s.indexOffset<<",\"index_bytes\":"<<s.indexBytes
           <<",\"vertex_stride\":"<<s.vertexStride<<",\"vertex_binding_offset\":"<<s.bindingOffset<<",\"index_format\":"<<s.indexFormat
           <<",\"position_format\":"<<s.position.format<<",\"position_slot\":"<<s.position.slot<<",\"position_offset\":"<<s.position.offset
           <<",\"position_step\":"<<s.position.step<<",\"capture_timing\":\"same_pre_draw_commands_as_cb_copy\"}";
        if(state_->discovery){auto code=state_->shaderCode.find(s.vsHash);
            D3D11_BUFFER_DESC vertexDesc{},indexDesc{};
            if(s.position.slot<32&&s.ia[s.position.slot])s.ia[s.position.slot]->GetDesc(&vertexDesc);
            if(s.ia[32])s.ia[32]->GetDesc(&indexDesc);
            const uint64_t requestedIndices=uint64_t(s.args.count)*(s.indexFormat==DXGI_FORMAT_R16_UINT?2u:4u);
            out<<",\"discovery_capture_bounds\":{\"vertex_source_byte_width\":"<<vertexDesc.ByteWidth
               <<",\"index_source_byte_width\":"<<indexDesc.ByteWidth<<",\"requested_index_bytes\":"<<requestedIndices
               <<",\"index_draw_truncated\":"<<(s.indexBytes<requestedIndices?"true":"false")
               <<",\"vertex_window_ends_before_source_end\":"<<(uint64_t(s.vertexOffset)+s.vertexBytes<vertexDesc.ByteWidth?"true":"false")
               <<",\"cb0_first_captured_row\":0,\"cb0_captured_rows\":8,\"other_cb_slots_captured\":false,\"uncaptured_geometry_semantics_verified\":false}";
            bool retained=false;
            if(code!=state_->shaderCode.end()){
                std::wstring path=state_->directory+std::wstring(s.vsHash.begin(),s.vsHash.end())+L".dxbc";
                HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(h==INVALID_HANDLE_VALUE)retained=GetLastError()==ERROR_FILE_EXISTS;
                else {DWORD count=0;retained=WriteFile(h,code->second.data(),DWORD(code->second.size()),&count,nullptr)&&count==code->second.size();CloseHandle(h);}
            }
            out<<",\"vs_bytecode_retained\":"<<(retained?"true":"false")
               <<",\"vs_bytecode_bytes\":"<<(code==state_->shaderCode.end()?0:code->second.size())
               <<",\"vs_bytecode_complete_sha256\":\""<<(retained?s.vsHash:"")<<"\",\"position_transform_semantics_verified\":false";
        }
    }
    static void BufferIdentity(std::ostream& out,ID3D11Buffer* buffer){
        D3D11_BUFFER_DESC d{};if(buffer)buffer->GetDesc(&d);out<<"{\"resource\":"<<Address(buffer)<<",\"descriptor_words\":";Raw(out,d);out<<'}';
    }
    static ComPtr<ID3D11Texture2D> Texture(ID3D11View* view){
        ComPtr<ID3D11Resource> r;if(view)view->GetResource(&r);ComPtr<ID3D11Texture2D> t;if(r)r.As(&t);return t;
    }
    static bool FullTexture(ID3D11Texture2D* t,unsigned w,unsigned h){
        D3D11_TEXTURE2D_DESC d{};if(t)t->GetDesc(&d);
        return t&&w&&h&&d.Width==w&&d.Height==h&&d.MipLevels==1&&d.ArraySize==1&&d.SampleDesc.Count==1;
    }
    static bool Viewport(ID3D11DeviceContext* c,unsigned w,unsigned h,D3D11_VIEWPORT* observed=nullptr){
        UINT n=16;D3D11_VIEWPORT v[16]{};c->RSGetViewports(&n,v);
        const bool full=n==1&&v[0].TopLeftX==0&&v[0].TopLeftY==0&&v[0].Width==float(w)&&v[0].Height==float(h);
        if(full&&observed)*observed=v[0];return full;
    }
    static bool PlainStages(ID3D11DeviceContext* c){
        ComPtr<ID3D11Predicate> p;BOOL condition=FALSE;c->GetPredication(&p,&condition);
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
        ID3D11Buffer* so[4]{};c->SOGetTargets(4,so);bool hasSO=false;for(auto* b:so)if(b){hasSO=true;b->Release();}
        return !p&&!gs&&!hs&&!ds&&!hasSO;
    }
    static void DrawState(std::ostream& out,ID3D11DeviceContext* c,Sample& sample){
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;c->VSGetShader(&vs,nullptr,nullptr);c->PSGetShader(&ps,nullptr,nullptr);
        out<<"\"vs\":"<<Address(vs.Get())<<",\"ps\":"<<Address(ps.Get());
        UINT n=16;D3D11_VIEWPORT vp[16]{};c->RSGetViewports(&n,vp);out<<",\"viewport_words\":[";
        for(UINT i=0;i<n&&i<16;++i){if(i)out<<',';Raw(out,vp[i]);}out<<']';
        n=16;D3D11_RECT sc[16]{};c->RSGetScissorRects(&n,sc);out<<",\"scissor_words\":[";
        for(UINT i=0;i<n&&i<16;++i){if(i)out<<',';Raw(out,sc[i]);}out<<']';
        ComPtr<ID3D11RasterizerState> rs;c->RSGetState(&rs);D3D11_RASTERIZER_DESC rd{};if(rs)rs->GetDesc(&rd);
        out<<",\"raster_state\":"<<Address(rs.Get())<<",\"raster_descriptor_words\":";Raw(out,rd);
        ComPtr<ID3D11DepthStencilState> depth;UINT stencil=0;c->OMGetDepthStencilState(&depth,&stencil);D3D11_DEPTH_STENCIL_DESC dd{};if(depth)depth->GetDesc(&dd);
        out<<",\"depth_state\":"<<Address(depth.Get())<<",\"stencil_ref\":"<<stencil<<",\"depth_descriptor_words\":";Raw(out,dd);
        ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;c->OMGetBlendState(&blend,factors,&mask);D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
        out<<",\"blend_state\":"<<Address(blend.Get())<<",\"sample_mask\":"<<mask<<",\"blend_factor_words\":";Raw(out,factors);
        out<<",\"blend_descriptor_words\":";Raw(out,bd);
        ComPtr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);D3D11_PRIMITIVE_TOPOLOGY topology{};c->IAGetPrimitiveTopology(&topology);
        out<<",\"input_layout\":"<<Address(layout.Get())<<",\"topology\":"<<unsigned(topology)<<",\"vertex_buffers\":[";
        ID3D11Buffer* buffers[32]{};UINT strides[32]{},offsets[32]{};c->IAGetVertexBuffers(0,32,buffers,strides,offsets);bool comma=false;
        for(unsigned i=0;i<32;++i)sample.ia[i].Attach(buffers[i]);
        for(unsigned i=0;i<32;++i){if(!buffers[i])continue;if(comma)out<<',';comma=true;
            out<<"{\"slot\":"<<i<<",\"stride\":"<<strides[i]<<",\"offset\":"<<offsets[i]<<",\"buffer\":";BufferIdentity(out,buffers[i]);out<<'}';}
        DXGI_FORMAT format{};UINT offset=0;c->IAGetIndexBuffer(&sample.ia[32],&format,&offset);
        out<<"],\"index_format\":"<<unsigned(format)<<",\"index_offset\":"<<offset<<",\"index_buffer\":";BufferIdentity(out,sample.ia[32].Get());
        out<<",\"render_targets\":[";ID3D11RenderTargetView* rt[8]{};c->OMGetRenderTargets(8,rt,nullptr);
        std::array<ComPtr<ID3D11RenderTargetView>,8> held;for(unsigned i=0;i<8;++i)held[i].Attach(rt[i]);
        for(unsigned i=0;i<8;++i){const auto& view=held[i];if(i)out<<',';auto t=Texture(view.Get());
            D3D11_RENDER_TARGET_VIEW_DESC vd{};if(view)view->GetDesc(&vd);out<<"{\"view\":"<<Address(view.Get())<<",\"resource\":"<<Address(t.Get())<<",\"view_words\":";Raw(out,vd);out<<'}';}
        out<<']';
    }
    void Write(const std::string& text)noexcept{
        auto& d=*state_;if(d.log==INVALID_HANDLE_VALUE)return;
        if(text.size()+1>MaxLogBytes-d.writtenBytes){Failure();return;}
        DWORD count=0;if(!WriteFile(d.log,text.data(),DWORD(text.size()),&count,nullptr)||count!=text.size()){Failure();return;}
        d.writtenBytes+=count;if(!WriteFile(d.log,"\n",1,&count,nullptr)||count!=1){Failure();return;}++d.writtenBytes;
    }
    static void SnapshotJson(std::ostream& out,const SnapshotRecord& s){
        out<<"{\"present\":"<<(s.present?"true":"false")<<",\"ambiguous\":"<<(s.ambiguous?"true":"false")
           <<",\"hdr_color\":"<<Address(s.color.Get())<<",\"source_depth\":"<<s.depth<<",\"observed_clear_generation\":"<<s.clearGeneration
           <<",\"copy_count\":"<<s.copyCount<<",\"dsv_words\":";Raw(out,s.view);out<<",\"width\":"<<s.width<<",\"height\":"<<s.height<<",\"geometry_ids\":[";
        for(unsigned i=0;i<s.geometryCount;++i){if(i)out<<',';out<<s.geometry[i];}
        out<<"],\"taa_id\":"<<s.taa<<",\"taa_count\":"<<s.taaCount<<'}';
    }
    void WriteSample(Sample& s){
        std::ostringstream out;Stream(out);out<<"{\"event\":\"sample\",\"id\":"<<s.id<<",\"kind\":\""<<(s.taa?"taa":"geometry")
            <<"\",\"epoch\":"<<s.epoch<<",\"draw_sequence\":"<<s.sequence<<",\"vs_sha256\":";Quote(out,s.vsHash);
        out<<",\"vs_generation\":"<<s.vsGeneration<<",\"ps_sha256\":";Quote(out,s.psHash);
        out<<",\"snapshot_index\":";if(s.taa)out<<s.snapshot;else out<<"null";
        out<<",\"metadata\":"<<s.metadata<<",\"submitted\":"<<(s.submitted?"true":"false")<<",\"retired\":"<<(s.retired?"true":"false")
           <<",\"valid\":"<<(s.valid?"true":"false")<<",\"reason\":\""<<s.reason<<"\",\"hresult\":"<<uint32_t(s.hr)<<",\"planes\":[";
        unsigned start=0;
        for(unsigned i=0;i<(s.taa?2u:1u);++i){if(i)out<<',';const auto& b=s.slice[i];
            out<<"{\"stage\":\""<<(s.taa&&i==0?"ps":"vs")<<"\",\"slot\":0,\"first_row\":"<<b.row
               <<",\"first_constant\":"<<b.first<<",\"constant_count\":"<<b.count<<",\"source_buffer\":"<<Address(b.source.Get())
               <<",\"source_byte_width\":"<<b.byteWidth<<",\"source_byte_offset\":"<<b.offset<<",\"byte_count\":"<<b.bytes;
            if(s.retired&&s.valid){const auto* words=s.words.data()+start/4;bool finite=true;
                for(unsigned j=0;j<b.bytes/4;++j)finite=finite&&((words[j]&0x7f800000u)!=0x7f800000u);
                const auto hash=Hash(words,b.bytes);if(hash.size()!=64)Failure();
                out<<",\"all_words_finite\":"<<(finite?"true":"false")<<",\"sha256\":\""<<hash<<"\",\"raw_words\":[";
                for(unsigned j=0;j<b.bytes/4;++j){if(j)out<<',';out<<words[j];}out<<"],\"float_rows\":[";
                for(unsigned j=0;j<b.bytes/4;++j){if(j){if(j%4==0)out<<"],[";else out<<',';}else out<<'[';
                    float v;std::memcpy(&v,&words[j],4);if(std::isfinite(v))out<<v;else out<<"null";}
                out<<"]]";}
            out<<'}';start+=b.bytes;
        }
        out<<']';WriteIA(out,s);out<<'}';Write(out.str());s.written=true;
    }
    bool GeometryJoined(const SnapshotRecord& s)const noexcept{
        if(!s.present||s.ambiguous||!s.geometryCount)return false;
        for(unsigned i=0;i<s.geometryCount;++i){const auto& sample=state_->samples[size_t(s.geometry[i]-1)];
            if(sample.taa||!sample.valid||!sample.retired||sample.epoch!=s.epoch)return false;}
        return true;
    }
    bool TaaJoined(const SnapshotRecord& s)const noexcept{
        if(!s.present||s.ambiguous||s.taaCount!=1||!s.taa)return false;
        const auto& sample=state_->samples[size_t(s.taa-1)];
        return sample.taa&&sample.valid&&sample.retired&&sample.epoch==s.epoch;
    }
    bool RangeJoined(const SnapshotRecord& s,DepthRange range)const noexcept{
        if(!s.present||s.ambiguous)return false;
        for(unsigned i=0;i<s.geometryCount;++i){const auto& sample=state_->samples[size_t(s.geometry[i]-1)];
            if(!sample.taa&&sample.valid&&sample.retired&&sample.epoch==s.epoch&&
               ClassifyDepthRange(sample.key.minDepthBits,sample.key.maxDepthBits)==range)return true;}
        return false;
    }
    static bool SameAssociation(const SnapshotRecord& a,const SnapshotRecord& b)noexcept{
        return a.present==b.present&&a.ambiguous==b.ambiguous&&a.epoch==b.epoch&&a.depth==b.depth&&
            a.clearGeneration==b.clearGeneration&&a.color.Get()==b.color.Get()&&a.width==b.width&&a.height==b.height&&
            a.copyCount==b.copyCount&&a.geometryCount==b.geometryCount&&a.geometry==b.geometry&&a.taa==b.taa&&a.taaCount==b.taaCount&&
            (!a.present||SameView(a.view,b.view));
    }
    void WriteAssociations(bool includeCurrent){
        auto& d=*state_;
        for(size_t e=0;e<EpochCount;++e){if(!includeCurrent&&d.epoch<=FirstEpoch+e)continue;
            for(unsigned index=0;index<MaxSnapshots;++index){auto& s=d.snapshots[e][index];if(!s.present||s.written)continue;
                std::ostringstream out;Stream(out);out<<"{\"event\":\"snapshot\",\"epoch\":"<<FirstEpoch+e<<",\"index\":"<<index<<",\"association\":";SnapshotJson(out,s);out<<'}';Write(out.str());s.written=true;}
            auto& p=d.pairs[e];if(p.present&&!p.written){std::ostringstream out;Stream(out);out<<"{\"event\":\"pair\",\"epoch\":"<<p.epoch<<",\"runtime_generation\":"<<p.runtimeGeneration
                <<",\"ambiguous\":"<<(p.ambiguous?"true":"false")<<",\"association_only\":true,\"eyes\":[";
                for(unsigned eye=0;eye<2;++eye){if(eye)out<<',';out<<"{\"eye\":"<<eye<<",\"source_index\":"<<p.source[eye]<<",\"association\":";SnapshotJson(out,p.eye[eye]);out<<'}';}
                out<<"]}";Write(out.str());p.written=true;}
        }
    }
    void Terminal(const char* reason){
        if(!state_||!state_->armed||state_->terminal)return;
        auto& d=*state_;
        for(size_t i=0;i<d.used;++i)if(!d.samples[i].written)WriteSample(d.samples[i]);
        WriteAssociations(true);
        bool geometryComplete=!d.regressed,taaComplete=!d.regressed,zeroComplete=!d.regressed,raisedComplete=!d.regressed;
        for(size_t e=0;e<EpochCount;++e){const auto& p=d.pairs[e];const bool paired=d.seen[e]&&p.present&&!p.ambiguous;
            geometryComplete=geometryComplete&&paired&&GeometryJoined(p.eye[0])&&GeometryJoined(p.eye[1]);
            taaComplete=taaComplete&&paired&&TaaJoined(p.eye[0])&&TaaJoined(p.eye[1]);
            zeroComplete=zeroComplete&&paired&&RangeJoined(p.eye[0],DepthRange::minimum_zero)&&RangeJoined(p.eye[1],DepthRange::minimum_zero);
            raisedComplete=raisedComplete&&paired&&RangeJoined(p.eye[0],DepthRange::minimum_positive)&&RangeJoined(p.eye[1],DepthRange::minimum_positive);}
        const bool complete=!d.failed&&!d.overflows&&geometryComplete&&taaComplete;
        std::ostringstream out;Stream(out);out<<"{\"event\":\"terminal\",\"status\":\""<<(complete?"complete":"incomplete")
           <<"\",\"reason\":\""<<reason<<"\",\"attempted_bundles\":"<<d.used<<",\"rejected_candidates\":"<<d.rejected
           <<",\"range_budget_rejections\":"<<d.rangeRejected<<",\"overflows\":"<<d.overflows<<",\"pending_resources_retained\":"<<(Pending()?"true":"false")
           <<",\"geometry_pairs_complete\":"<<(geometryComplete?"true":"false")<<",\"taa_pairs_complete\":"<<(taaComplete?"true":"false")
           <<",\"minimum_zero_pairs_complete\":"<<(zeroComplete?"true":"false")<<",\"minimum_positive_pairs_complete\":"<<(raisedComplete?"true":"false")
           <<",\"capture_and_association_only\":true,\"opaque_geometry_verified\":false}";Write(out.str());d.terminal=true;
        if(d.log!=INVALID_HANDLE_VALUE){CloseHandle(d.log);d.log=INVALID_HANDLE_VALUE;}
    }
    void Arm(ID3D11DeviceContext* context){
        auto& d=*state_;d.checked=true;Mode();
        wchar_t path[32768]{};const DWORD length=GetEnvironmentVariableW(L"ETS2_GEOMETRY_TAA_PROBE",path,DWORD(std::size(path)));
        if(!length||length>=std::size(path))return;
        wchar_t epochText[32]{};const DWORD epochLength=GetEnvironmentVariableW(L"ETS2_INPUT_CAPTURE_EPOCH",epochText,DWORD(std::size(epochText)));
        if(epochLength){
            if(epochLength>=std::size(epochText))return;uint64_t parsed=0;
            for(DWORD i=0;i<epochLength;++i){if(epochText[i]<L'0'||epochText[i]>L'9'||parsed>1000000)return;parsed=parsed*10+uint64_t(epochText[i]-L'0');}
            if(!parsed||parsed>1000000)return;FirstEpoch=parsed;
        }
        if(!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        ContextLock lock(context);if(!lock.held)return;
        if(!CreateDirectoryW(path,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return;
        const DWORD attributes=GetFileAttributesW(path);
        if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
        std::wstring output(path);if(output.back()!=L'\\'&&output.back()!=L'/')output+=L'\\';d.directory=output;output+=L"geometry-taa-probe.jsonl";
        d.log=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(d.log==INVALID_HANDLE_VALUE)return;
        d.context=context;d.armed=true;
        std::ostringstream out;Stream(out);out<<"{\"event\":\"session\",\"schema\":2,\"pid\":"<<GetCurrentProcessId()<<",\"context\":"<<Address(context)
            <<",\"first_epoch\":"<<FirstEpoch<<",\"epoch_count\":16,\"maximum_staging_bundles\":256,\"maximum_staging_bytes\":33652736,\"maximum_ia_bytes_per_sample\":131328,\"maximum_geometry_ids_per_snapshot\":6,\"range_policy\":\"first_per_exact_min_max_float_bits;per_epoch_depth_view_clear_VS_reserve_two_minimum_zero_ranges_and_one_minimum_positive_range;0<=min<max<=1\",\"range_class_is_not_scene_classification\":true,\"opaque_geometry_verified\":false,\"sample_valid_semantics\":\"retired_readback_only_no_semantic_validation\",\"clear_coverage\":\"observed_depth_clear_callbacks_since_resource_registration_only\",\"taa_offset_units\":\"TAA_current_t1_UV_displacement_not_verified_NGX_jitter\",\"state_descriptor_zero_object\":\"D3D11_default_state_descriptor_words_not_applicable\",\"getdata_donotflush\":true,\"map_donotwait\":true}";Write(out.str());
        if(d.epoch!=UINT64_MAX&&d.epoch>=FirstEpoch){d.failed=true;Terminal("armed-after-fixed-window-start");}
        if(d.discovery)Write("{\"event\":\"discovery_policy\",\"epoch_count\":2,\"maximum_bundles\":128,\"maximum_vertex_bytes\":49152,\"maximum_index_bytes\":4096,\"maximum_staging_and_readback_bytes\":13697024,\"maximum_vs_bytecode_bytes\":2097152,\"depth_writer_required\":true,\"per_depth_generation_reserve\":\"16 per exact viewport min/max bit pair;32 total per depth/view/clear/epoch;distinct VS/indexed-draw tuples\",\"unknown_vs_position_semantics\":true}");
    }
public:
    void ShaderCode(const std::string& hash,const void* bytes,size_t size)noexcept try{
        Mode();if(!state_||!state_->discovery||!bytes||!size||size>16384||state_->shaderCode.count(hash)||
            size>DiscoveryCodeBytes-state_->shaderCodeBytes||Hash(bytes,unsigned(size))!=hash)return;
        const auto* first=static_cast<const unsigned char*>(bytes);state_->shaderCode[hash]=std::vector<unsigned char>(first,first+size);state_->shaderCodeBytes+=size;
    }catch(...){Failure();}
    void Layout(uint64_t id,const PositionLayout& layout)noexcept try{
        if(!state_)return;state_->layouts.erase(id);if(state_->layouts.size()<2048)state_->layouts[id]=layout;
    }catch(...){Failure();}
    void EraseLayout(uint64_t id)noexcept{if(state_)state_->layouts.erase(id);}
    Probe()=default;
    Probe(const Probe&)=delete;Probe& operator=(const Probe&)=delete;
    ~Probe(){
        if(!state_)return;
        try{Terminal("owner-destroyed");}catch(...){}
        if(state_->log!=INVALID_HANDLE_VALUE){CloseHandle(state_->log);state_->log=INVALID_HANDLE_VALUE;}
        // Intentionally quarantine the complete bounded owner. No COM object
        // referenced by an unproven GPU copy is destroyed during cancellation.
        if(Pending())(void)state_.release();
    }
    void Epoch(uint64_t epoch)noexcept{
        if(!state_)return;auto& d=*state_;
        if(d.epoch!=UINT64_MAX&&epoch<d.epoch){d.regressed=true;d.failed=true;}
        d.epoch=epoch;
        if(d.armed&&InEpochWindow(epoch)){d.seen[size_t(epoch-FirstEpoch)]=true;if(!d.startTick)d.startTick=GetTickCount64();}
    }
    void Geometry(ID3D11DeviceContext* c,uint64_t sequence,const std::string& vsHash,uint64_t vsGeneration,
                  const std::string& psHash,const DrawArgs& args,unsigned expectedW,unsigned expectedH)noexcept try{
        if(!Active()||!args.indexed||!args.count||!args.instances||(!state_->discovery&&vsHash!=WorldCandidate&&vsHash!=CabCandidate))return;
        if(!Context(c))return;ContextLock lock(c);if(!lock.held){Reject();return;}
        ComPtr<ID3D11DepthStencilView> view;c->OMGetRenderTargets(0,nullptr,&view);auto depth=Texture(view.Get());
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};if(view)view->GetDesc(&vd);D3D11_VIEWPORT viewport{};
        if(!depth||vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||vd.Texture2D.MipSlice!=0||
           !FullTexture(depth.Get(),expectedW,expectedH)||!Viewport(c,expectedW,expectedH,&viewport)||!PlainStages(c)){Reject();return;}
        auto* record=FindDepth(depth.Get(),true);if(!record)return;
        GeometryKey key{state_->epoch,Address(depth.Get()),record->generation,vd,vsHash,FloatBits(viewport.MinDepth),FloatBits(viewport.MaxDepth)};
        const auto range=ClassifyDepthRange(key.minDepthBits,key.maxDepthBits);unsigned zeroCount=0,raisedCount=0,exactRangeCount=0,snapshotCount=0;
        if(state_->discovery){
            ComPtr<ID3D11DepthStencilState> ds;UINT reference=0;c->OMGetDepthStencilState(&ds,&reference);
            D3D11_DEPTH_STENCIL_DESC desc{};if(ds)ds->GetDesc(&desc);
            if(!ds||!desc.DepthEnable||desc.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL||
                !state_->shaderCode.count(vsHash)||state_->used>=DiscoveryBundles){Reject();return;}
        }
        for(size_t i=0;i<state_->used;++i){const auto& sample=state_->samples[i];if(sample.taa)continue;
            if(state_->discovery){
                if(!MatchesSnapshot(sample.key,key.epoch,key.depth,key.clearGeneration,key.view))continue;
                ++snapshotCount;if(sample.key.minDepthBits==key.minDepthBits&&sample.key.maxDepthBits==key.maxDepthBits)++exactRangeCount;
                if(SameGeometryKey(sample.key,key)&&sample.args.count==args.count&&sample.args.instances==args.instances&&
                    sample.args.first==args.first&&sample.args.baseVertex==args.baseVertex&&sample.args.firstInstance==args.firstInstance)return;
            }else {if(!SameGeometrySource(sample.key,key))continue;if(SameGeometryKey(sample.key,key))return;}
            const auto previousRange=ClassifyDepthRange(sample.key.minDepthBits,sample.key.maxDepthBits);
            if(previousRange==DepthRange::minimum_zero)++zeroCount;else if(previousRange==DepthRange::minimum_positive)++raisedCount;
        }
        const bool budget=state_->discovery?(range!=DepthRange::unsupported&&exactRangeCount<16&&snapshotCount<32):RangeBudgetAvailable(range,zeroCount,raisedCount);
        if(!budget){++state_->rangeRejected;Reject();return;}
        auto* sample=Allocate(false,sequence);if(!sample)return;sample->key=key;sample->vsHash=vsHash;sample->vsGeneration=vsGeneration;sample->psHash=psHash;sample->args=args;
        std::ostringstream out;Stream(out);out<<"{\"opaque_geometry_verified\":false,\"dsv\":"<<Address(view.Get())<<",\"source_depth\":"<<Address(depth.Get())
            <<",\"observed_clear_generation\":"<<record->generation<<",\"dsv_words\":";Raw(out,vd);
        out<<",\"viewport_min_depth_bits\":"<<key.minDepthBits<<",\"viewport_max_depth_bits\":"<<key.maxDepthBits
           <<",\"depth_range_class\":\""<<(range==DepthRange::minimum_zero?"minimum_zero":"minimum_positive")
           <<"\",\"width\":"<<expectedW<<",\"height\":"<<expectedH<<",\"indexed\":true,\"count\":"<<args.count<<",\"instances\":"<<args.instances
           <<",\"first\":"<<args.first<<",\"base_vertex\":"<<args.baseVertex<<",\"first_instance\":"<<args.firstInstance<<',';
        DrawState(out,c,*sample);out<<'}';sample->metadata=out.str();Queue(c,*sample);
    }catch(...){Failure();}
    void Snapshot(unsigned index,ID3D11Texture2D* oldColor,ID3D11Texture2D* oldDepth,const D3D11_DEPTH_STENCIL_VIEW_DESC& view)noexcept try{
        if(!Active())return;if(index>=MaxSnapshots||!oldColor||!oldDepth||!SameView(view,view)){Reject();return;}
        auto* depth=FindDepth(oldDepth,true);if(!depth)return;
        auto& s=state_->snapshots[size_t(state_->epoch-FirstEpoch)][index];
        if(s.present){
            if(s.color.Get()!=oldColor||s.depth!=Address(oldDepth)||s.clearGeneration!=depth->generation||!SameView(s.view,view)||s.referenced){s.ambiguous=true;return;}
            // Tracker permits same-source refresh only before lineage use.
            // Mirror that latest actual depth copy, not the earlier empty copy.
            s.geometryCount=0;s.geometry={};
        }
        s.present=true;s.epoch=state_->epoch;s.color=oldColor;s.depth=Address(oldDepth);s.clearGeneration=depth->generation;s.view=view;
        ++s.copyCount;
        D3D11_TEXTURE2D_DESC d{};oldDepth->GetDesc(&d);s.width=d.Width;s.height=d.Height;
        for(size_t i=0;i<state_->used;++i){const auto& candidate=state_->samples[i];if(candidate.taa||!MatchesSnapshot(candidate.key,s.epoch,s.depth,s.clearGeneration,view))continue;
            if(s.geometryCount==s.geometry.size()){s.ambiguous=true;break;}s.geometry[s.geometryCount++]=candidate.id;}
    }catch(...){Failure();}
    void Cleared(ID3D11Resource* resource)noexcept{
        if(!state_||!state_->armed||state_->terminal||state_->epoch>=FirstEpoch+EpochCount)return;
        if(auto* d=FindDepth(resource,false)){if(d->generation==UINT64_MAX){Failure();return;}++d->generation;}
    }
    void Taa(ID3D11DeviceContext* c,uint64_t sequence,unsigned index,ID3D11Texture2D* sourceColor,ID3D11Texture2D* targetColor,
             const std::string& vsHash,uint64_t vsGeneration,const std::string& psHash)noexcept try{
        if(!Active()||state_->discovery)return;if(index>=MaxSnapshots||!Context(c)){Reject();return;}
        ContextLock lock(c);if(!lock.held){Reject();return;}
        auto& snapshot=state_->snapshots[size_t(state_->epoch-FirstEpoch)][index];
        if(!snapshot.present||!FullTexture(sourceColor,snapshot.width,snapshot.height)||!FullTexture(targetColor,snapshot.width,snapshot.height)||
           !Viewport(c,snapshot.width,snapshot.height)||!PlainStages(c)){Reject();return;}
        ID3D11RenderTargetView* raw[8]{};ComPtr<ID3D11DepthStencilView> dsv;c->OMGetRenderTargets(8,raw,&dsv);
        std::array<ComPtr<ID3D11RenderTargetView>,8> rt;for(unsigned i=0;i<8;++i)rt[i].Attach(raw[i]);
        const auto target=Texture(rt[0].Get()),history=Texture(rt[1].Get());
        bool valid=!dsv&&target.Get()==targetColor&&history&&history.Get()!=targetColor&&history.Get()!=sourceColor&&sourceColor!=targetColor;
        for(unsigned i=2;i<8;++i)valid=valid&&!rt[i];
        D3D11_TEXTURE2D_DESC td{},hd{};if(target)target->GetDesc(&td);if(history)history->GetDesc(&hd);
        valid=valid&&td.Format==DXGI_FORMAT_R16G16B16A16_FLOAT&&hd.Format==td.Format&&FullTexture(history.Get(),snapshot.width,snapshot.height);
        ComPtr<ID3D11ShaderResourceView> current;c->PSGetShaderResources(1,1,&current);const auto boundSource=Texture(current.Get());
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};if(current)current->GetDesc(&sv);
        valid=valid&&boundSource.Get()==sourceColor&&sv.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&sv.Texture2D.MostDetailedMip==0&&sv.Texture2D.MipLevels==1;
        if(!valid){Reject();return;}
        auto* sample=Allocate(true,sequence);if(!sample)return;sample->snapshot=index;sample->vsHash=vsHash;sample->vsGeneration=vsGeneration;sample->psHash=psHash;
        snapshot.referenced=true;++snapshot.taaCount;if(!snapshot.taa)snapshot.taa=sample->id;
        std::ostringstream out;Stream(out);out<<"{\"source_color\":"<<Address(sourceColor)<<",\"target_color\":"<<Address(targetColor)
            <<",\"current_t1_view_words\":";Raw(out,sv);out<<",\"source_snapshot\":";SnapshotJson(out,snapshot);out<<',';DrawState(out,c,*sample);
        out<<",\"pixel_resources\":[";bool comma=false;
        for(unsigned slot:{0u,1u,3u,4u,15u}){ComPtr<ID3D11ShaderResourceView> v;c->PSGetShaderResources(slot,1,&v);auto t=Texture(v.Get());D3D11_SHADER_RESOURCE_VIEW_DESC desc{};if(v)v->GetDesc(&desc);
            if(comma)out<<',';comma=true;out<<"{\"slot\":"<<slot<<",\"view\":"<<Address(v.Get())<<",\"resource\":"<<Address(t.Get())<<",\"view_words\":";Raw(out,desc);out<<'}';}
        out<<"]}";sample->metadata=out.str();Queue(c,*sample);
    }catch(...){Failure();}
    // Caller contract: invoke only after the current synchronous exact matcher
    // accepted both distinct eyes. The probe records that association; it does
    // not independently reproduce the matcher or certify geometric semantics.
    void Pair(ID3D11DeviceContext* c,uint64_t epoch,uint64_t runtimeGeneration,const unsigned(&sourceIndices)[2])noexcept try{
        if(!Active()||epoch!=state_->epoch)return;
        if(!Context(c)||sourceIndices[0]>=MaxSnapshots||sourceIndices[1]>=MaxSnapshots||sourceIndices[0]==sourceIndices[1]||!runtimeGeneration){Reject();return;}
        auto& p=state_->pairs[size_t(epoch-FirstEpoch)];
        if(p.present){
            if(p.runtimeGeneration!=runtimeGeneration||p.source[0]!=sourceIndices[0]||p.source[1]!=sourceIndices[1]||
               !SameAssociation(p.eye[0],state_->snapshots[size_t(epoch-FirstEpoch)][sourceIndices[0]])||
               !SameAssociation(p.eye[1],state_->snapshots[size_t(epoch-FirstEpoch)][sourceIndices[1]]))p.ambiguous=true;
            return;
        }
        p.present=true;p.epoch=epoch;p.runtimeGeneration=runtimeGeneration;
        for(unsigned eye=0;eye<2;++eye){p.source[eye]=sourceIndices[eye];auto& s=state_->snapshots[size_t(epoch-FirstEpoch)][sourceIndices[eye]];s.referenced=true;p.eye[eye]=s;}
    }catch(...){Failure();}
    void Poll(ID3D11DeviceContext* c)noexcept try{
        if(!state_)return;auto& d=*state_;if(!d.checked)Arm(c);if(!d.armed||d.terminal)return;
        if(!Context(c)){Terminal("context-changed");return;}ContextLock lock(c);if(!lock.held){Failure();Terminal("context-protection-unavailable");return;}
        for(size_t i=0;i<d.used;++i){auto& s=d.samples[i];
            if(s.submitted&&!s.done){
                if(!s.retired){const HRESULT hr=c->GetData(s.query.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if(hr==S_FALSE)continue;if(hr!=S_OK){SampleFailure(s,"event-query-failed",hr);}else s.retired=true;}
                if(s.retired&&!s.done){D3D11_MAPPED_SUBRESOURCE mapped{};const HRESULT hr=c->Map(s.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                    if(hr==DXGI_ERROR_WAS_STILL_DRAWING)continue;
                    if(FAILED(hr))SampleFailure(s,"staging-map-failed",hr);
                    else {std::memcpy(s.words.data(),mapped.pData,s.bytes);c->Unmap(s.staging.Get(),0);s.done=true;s.valid=true;s.reason="copied";
                        if(s.iaSubmitted){D3D11_MAPPED_SUBRESOURCE ia{};const HRESULT iahr=c->Map(s.iaStaging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&ia);
                            if(iahr==DXGI_ERROR_WAS_STILL_DRAWING){s.done=false;continue;}
                            if(FAILED(iahr))s.iaReason="ia-map-failed";
                            else {s.iaData.resize(s.vertexBytes+s.indexBytes);std::memcpy(s.iaData.data(),ia.pData,s.iaData.size());c->Unmap(s.iaStaging.Get(),0);s.iaValid=true;s.iaReason="copied";}
                        }
                    }
                }
            }
            if(s.done&&!s.written)WriteSample(s);
        }
        WriteAssociations(false);
        if(d.regressed){Terminal("epoch-regressed");return;}
        const bool timedOut=d.startTick&&(GetTickCount64()-d.startTick>=TimeoutMs);
        bool outstanding=false;for(size_t i=0;i<d.used;++i)outstanding=outstanding||(d.samples[i].submitted&&!d.samples[i].done);
        if(timedOut){Failure();Terminal("fixed-window-timeout");}
        else if(d.epoch>=FirstEpoch+(d.discovery?DiscoveryEpochs:EpochCount)&&!outstanding)Terminal("fixed-window-ended");
    }catch(...){Failure();try{Terminal("poll-exception");}catch(...){}}
};
} // namespace ets2_geometry_taa
