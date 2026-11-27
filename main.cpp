#define UNICODE
#define _UNICODE
#include <windows.h>
#include <imm.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib,"gdiplus.lib")
#include <commctrl.h>
#pragma comment(lib,"comctl32.lib")
#include <uxtheme.h>
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#include <commdlg.h>
#include <windowsx.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#pragma comment(lib,"oleaut32.lib")
#pragma comment(lib,"uuid.lib")
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#pragma comment(lib,"windowsapp.lib")
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using namespace std;
namespace fs=std::filesystem;
COLORREF background=RGB(255,247,213); HWND edit,bar,syncNotice;
bool lastSyncSucceeded=true;
void showSyncResult(HWND w,bool success){
    lastSyncSucceeded=success;
    SetWindowText(syncNotice,success?L"同步成功":L"同步失败");
    SetWindowPos(syncNotice,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    InvalidateRect(syncNotice,nullptr,TRUE);
    SetTimer(w,3,success?1800:3200,0);
} HFONT ui,scriptFont; fs::path base,file; bool dirty=false,dark=false,loading=true; HANDLE syncJob=nullptr;
ULONGLONG syncAnimationStart=0;
DWORD CALLBACK streamOut(DWORD_PTR c,LPBYTE p,LONG n,LONG* done){((string*)c)->append((char*)p,n);*done=n;return 0;}
struct Reader{string data;size_t pos=0;};
DWORD CALLBACK streamIn(DWORD_PTR c,LPBYTE p,LONG n,LONG* done){auto&r=*(Reader*)c;*done=(LONG)min<size_t>(n,r.data.size()-r.pos);memcpy(p,r.data.data()+r.pos,*done);r.pos+=*done;return 0;}
COLORREF colors[16]{};
int barBrightness=86;
COLORREF barColor(){
    return RGB(GetRValue(background)*barBrightness/100,
               GetGValue(background)*barBrightness/100,
               GetBValue(background)*barBrightness/100);
}
bool portableColors=false;
COLORREF trackColor,thumbColor;
COLORREF blend(COLORREF source,int target,int percent){
    auto channel=[&](int c){return (c*(100-percent)+target*percent)/100;};
    return RGB(channel(GetRValue(source)),channel(GetGValue(source)),channel(GetBValue(source)));
}
void deriveScrollColors(){
    trackColor=blend(background,255,58);
    thumbColor=blend(background,0,45);
}
void readColors(const string& json){
    auto number=[&](string name,unsigned long fallback,unsigned long limit){
        string key="\""+name+"\":";
        auto where=json.find(key);
        if(where==string::npos)return fallback;
        const char* start=json.c_str()+where+key.size();
        char* end;auto value=strtoul(start,&end,10);
        return end!=start && *end==',' && value<=limit?value:fallback;
    };
    portableColors=json.find("\"barBrightness\":")!=string::npos;
    barBrightness=(int)number("barBrightness",barBrightness,100);
    deriveScrollColors();
    trackColor=number("scrollTrack",trackColor,0xffffff);
    thumbColor=number("scrollThumb",thumbColor,0xffffff);
    for(int i=0;i<16;++i)colors[i]=number("custom"+to_string(i),colors[i],0xffffff);
}
using namespace winrt::Windows::Data::Json;
wstring languageFaces[2];
constexpr UINT defaultSaveDelaySeconds=5;
UINT saveDelaySeconds=defaultSaveDelaySeconds;
constexpr UINT defaultSyncDelayMinutes=10;
UINT syncDelayMinutes=defaultSyncDelayMinutes;
bool migrateLegacy=false;
wstring editorText(){
    wstring text(GetWindowTextLength(edit)+1,L'\0');
    text.resize(GetWindowText(edit,text.data(),(int)text.size()));
    return text;
}
string jsonQuote(wstring_view value){
    return winrt::to_string(JsonValue::CreateStringValue(value).Stringify());
}
string colorText(COLORREF value){
    char buf[8];sprintf_s(buf,"#%02X%02X%02X",GetRValue(value),GetGValue(value),GetBValue(value));
    return buf;
}
COLORREF parseColor(winrt::hstring value){
    if(value.size()!=7 || value[0]!=L'#' || wcsspn(value.c_str()+1,L"0123456789abcdefABCDEF")!=6)
        throw invalid_argument("color");
    auto n=wcstoul(value.c_str()+1,nullptr,16);
    return RGB(n>>16,n>>8,n);
}
winrt::com_ptr<ITextDocument> textDocument(){
    winrt::com_ptr<IRichEditOle> ole;
    SendMessage(edit,EM_GETOLEINTERFACE,0,(LPARAM)ole.put());
    if(!ole)throw runtime_error("RichEdit document unavailable");
    return ole.as<ITextDocument>();
}
CHARFORMAT2 readStyle(const JsonObject& value){
    CHARFORMAT2 cf{sizeof(cf)};cf.dwMask=CFM_FACE|CFM_SIZE|CFM_COLOR;
    auto face=value.GetNamedString(L"fontFamily");auto size=value.GetNamedNumber(L"fontSizePt");
    if(face.size()>=LF_FACESIZE || size<=0 || size>1638)throw invalid_argument("font");
    wcscpy_s(cf.szFaceName,face.c_str());cf.yHeight=(LONG)(size*20+0.5);
    auto color=value.GetNamedString(L"textColor");
    if(color==L"auto")cf.dwEffects=CFE_AUTOCOLOR;else cf.crTextColor=parseColor(color);
    return cf;
}
void writeHex(ostream& out,string_view bytes){
    constexpr char digits[]="0123456789abcdef";
    char buffer[8192];
    size_t used=0;
    for(unsigned char byte:bytes){
        buffer[used++]=digits[byte>>4];buffer[used++]=digits[byte&15];
        if(used==sizeof(buffer)){out.write(buffer,used);used=0;}
    }
    out.write(buffer,used);
}
string hexDecode(wstring_view hex){
    if(hex.size()%2)throw invalid_argument("hex length");
    auto nibble=[](wchar_t c)->int{
        if(c>=L'0' && c<=L'9')return c-L'0';
        if(c>=L'a' && c<=L'f')return c-L'a'+10;
        if(c>=L'A' && c<=L'F')return c-L'A'+10;
        throw invalid_argument("hex digit");
    };
    string bytes;bytes.reserve(hex.size()/2);
    for(size_t i=0;i<hex.size();i+=2)bytes.push_back((char)((nibble(hex[i])<<4)|nibble(hex[i+1])));
    return bytes;
}
bool save()try{
    if(!dirty)return true;
    string r;EDITSTREAM stream{(DWORD_PTR)&r,0,streamOut};
    SendMessage(edit,EM_STREAMOUT,SF_RTF,(LPARAM)&stream);
    if(stream.dwError || r.empty())return false;
    // Preserve every RTF byte, including any code-page-specific bytes.

    auto tmp=file;tmp+=L".tmp";ofstream out(tmp,ios::binary);
    out<<u8R"({
  "_说明": {
    "_说明": "字段说明，可随备份一起保留。",
    "version": "文件格式版本。",
    "appVersion": "写入此文件的应用版本。",
    "saveDelaySeconds": "最后一次修改到本地自动保存的秒数，正整数，默认 5 秒。",
    "syncDelayMinutes": "最后一次修改到 Gitee 自动同步的分钟数，正整数，默认 10 分钟。",
    "richText": "原始 RTF 字节的十六进制编码，用于还原字体、字号、行距、列表等排版。",
    "chineseFont": "中文字体，应用于整张便笺的中文及后续输入；字号、颜色和样式按选区设置。空值沿用编辑器字体。",
    "englishFont": "英文字体，应用于整张便笺的英文及后续输入；字号、颜色和样式按选区设置。空值沿用编辑器字体。",
    "background": "便笺背景色，格式为 #RRGGBB。",
    "barBrightness": "底栏相对背景的亮度百分比。",
    "barColor": "根据背景和底栏亮度计算的颜色，供预览。",
    "scrollTrack": "滚动条轨道颜色。",
    "scrollThumb": "滚动条滑块颜色。",
    "palette": "颜色选择器的 16 个自定义色位，按顺序排列。"
  },
  "version": 4,
  "appVersion": "1.0.1",
  "saveDelaySeconds": )"<<saveDelaySeconds<<",\n  \"syncDelayMinutes\": "<<syncDelayMinutes<<",\n  \"richText\": "<<'"';
    writeHex(out,r);out<<'"';
    out<<",\n  \"chineseFont\": "<<jsonQuote(languageFaces[1])<<",\n  \"englishFont\": "<<jsonQuote(languageFaces[0]);
    out<<",\n  \"background\": \""<<colorText(background)<<"\",\n  \"barBrightness\": "<<barBrightness
       <<",\n  \"barColor\": \""<<colorText(barColor())<<"\",\n  \"scrollTrack\": \""<<colorText(trackColor)
       <<"\",\n  \"scrollThumb\": \""<<colorText(thumbColor)<<"\",\n  \"palette\": [";
    for(int i=0;i<16;i++)out<<(i?", ":"")<<'"'<<colorText(colors[i])<<'"';
    out<<"]\n}\n";out.close();
    if(!out||!MoveFileEx(tmp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
    dirty=false;migrateLegacy=false;return true;
}catch(...){return false;}
bool load()try{
    if(!fs::exists(file))return true;
    ifstream input(file,ios::binary);string x((istreambuf_iterator<char>(input)),{});
    if(x.rfind("\xef\xbb\xbf",0)==0)x.erase(0,3);
    auto json=JsonObject::Parse(winrt::to_hstring(x));
    auto version=json.GetNamedNumber(L"version");
    if(version==2 || version==3 || version==4){
        winrt::hstring text;if(version<4)text=json.GetNamedString(L"text");
        auto rich=json.GetNamedString(L"richText");
        Reader r;
        if(version>=3){if(version==3)text=winrt::to_hstring(hexDecode(text));r.data=hexDecode(rich);}
        else for(auto c:rich){if(c>255)return false;r.data.push_back((char)c);}
        auto palette=json.GetNamedArray(L"palette");
        if(palette.Size()!=16 || r.data.rfind("{\\rtf",0)!=0)return false;
        COLORREF swatches[16];for(int i=0;i<16;i++)swatches[i]=parseColor(palette.GetStringAt(i));
        auto bg=parseColor(json.GetNamedString(L"background"));
        auto track=parseColor(json.GetNamedString(L"scrollTrack"));
        auto thumb=parseColor(json.GetNamedString(L"scrollThumb"));
        double minutes=json.GetNamedNumber(L"syncDelayMinutes",(double)defaultSyncDelayMinutes);
        if(!std::isfinite(minutes) || minutes<1 || minutes>USER_TIMER_MAXIMUM/60000 || minutes!=std::floor(minutes))minutes=defaultSyncDelayMinutes;
        double delay=json.GetNamedNumber(L"saveDelaySeconds",(double)defaultSaveDelaySeconds);
        if(!std::isfinite(delay) || delay<1 || delay>USER_TIMER_MAXIMUM/1000 || delay!=std::floor(delay))delay=defaultSaveDelaySeconds;
        auto brightness=json.GetNamedNumber(L"barBrightness");
        if(brightness<0||brightness>100)return false;
        bool hasStyles=version<4 && json.HasKey(L"defaultStyle") && json.HasKey(L"textStyles");
        if(hasStyles){auto def=readStyle(json.GetNamedObject(L"defaultStyle"));SendMessage(edit,EM_SETCHARFORMAT,SCF_DEFAULT,(LPARAM)&def);}
        EDITSTREAM stream{(DWORD_PTR)&r,0,streamIn};SendMessage(edit,EM_STREAMIN,SF_RTF,(LPARAM)&stream);
        if(stream.dwError)return false;
        if(version<4 && editorText()!=wstring_view(text))SetWindowText(edit,text.c_str());
        else if(hasStyles){
            auto styles=json.GetNamedArray(L"textStyles");
            for(auto item:styles){
                auto value=item.GetObject();auto cf=readStyle(value);
                CHARRANGE range{(LONG)value.GetNamedNumber(L"start"),(LONG)value.GetNamedNumber(L"end")};
                if(range.cpMin<0 || range.cpMax<=range.cpMin || range.cpMax>(LONG)text.size())return false;
                SendMessage(edit,EM_EXSETSEL,0,(LPARAM)&range);
                SendMessage(edit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
            }
            SendMessage(edit,EM_SETSEL,0,0);
        }
        languageFaces[0]=json.GetNamedString(L"englishFont",L"");
        languageFaces[1]=json.GetNamedString(L"chineseFont",L"");
        background=bg;trackColor=track;thumbColor=thumb;barBrightness=(int)brightness;
        copy_n(swatches,16,colors);portableColors=true;saveDelaySeconds=(UINT)delay;syncDelayMinutes=(UINT)minutes;migrateLegacy=version<4 || !json.HasKey(L"saveDelaySeconds") || !json.HasKey(L"syncDelayMinutes");return true;
    }
    if(version!=1)return false;
    migrateLegacy=true;auto a=x.find("\"rtfHex\":\"");if(a==x.npos)return false;a+=10;auto b=x.find('"',a);if(b==x.npos||(b-a)%2)return false;Reader r;string digits="0123456789abcdef";while(a<b){auto i=digits.find(x[a++]),j=digits.find(x[a++]);if(i>15||j>15)return false;r.data+=(char)(i*16+j);}if(r.data.rfind("{\\rtf",0)!=0)return false;dark=x.find("\"dark\":true")!=x.npos; background=dark?RGB(201,196,219):RGB(255,247,213); auto key=x.find("\"background\":"); if(key!=x.npos){char* end; auto value=strtoul(x.c_str()+key+13,&end,10); if(*end==',' && value<=0xffffff)background=value;} readColors(x); EDITSTREAM s{(DWORD_PTR)&r,0,streamIn};SendMessage(edit,EM_STREAMIN,SF_RTF,(LPARAM)&s);return !s.dwError;}catch(...){return false;}

std::unique_ptr<Gdiplus::Bitmap> syncFrame;
ULONG_PTR iconGraphics=0;
HICON themedIcons[2]{};
COLORREF iconColor=CLR_INVALID;
UINT iconDpi=0;
HICON makeNoteIcon(int size){
    using namespace Gdiplus;
    Bitmap bitmap(size,size,PixelFormat32bppARGB);
    {
        Graphics canvas(&bitmap);
        canvas.Clear(Color(0,0,0,0));
        canvas.SetSmoothingMode(SmoothingModeAntiAlias);
        canvas.ScaleTransform(size/64.f,size/64.f);
        auto tint=[](COLORREF c){return Color(255,GetRValue(c),GetGValue(c),GetBValue(c));};
        int luminance=299*GetRValue(background)+587*GetGValue(background)+114*GetBValue(background);
        int contrast=luminance>110000?0:255;
        SolidBrush paper(tint(background)),fold(tint(blend(background,contrast,30))),
                   ink(tint(blend(background,contrast,65)));
        Point body[]={{6,6},{58,6},{58,40},{42,56},{6,56}};
        Point corner[]={{42,40},{58,40},{42,56}};
        canvas.FillPolygon(&paper,body,5);
        canvas.FillPolygon(&fold,corner,3);
        canvas.FillRectangle(&ink,15,19,33,4);
        canvas.FillRectangle(&ink,15,30,23,4);
    }
    HICON icon=nullptr;
    return bitmap.GetHICON(&icon)==Ok?icon:nullptr;
}
void updateNoteIcons(HWND window){
    UINT dpi=GetDpiForWindow(window);
    if(iconColor==background && iconDpi==dpi)return;
    if(!iconGraphics){
        Gdiplus::GdiplusStartupInput options;
        if(Gdiplus::GdiplusStartup(&iconGraphics,&options,nullptr)!=Gdiplus::Ok)return;
    }
    HICON next[]={makeNoteIcon(GetSystemMetricsForDpi(SM_CXSMICON,dpi)),
                  makeNoteIcon(GetSystemMetricsForDpi(SM_CXICON,dpi))};
    if(next[0] && next[1]){
        for(int i=0;i<2;++i){
            SendMessage(window,WM_SETICON,i==0?ICON_SMALL:ICON_BIG,(LPARAM)next[i]);
            swap(themedIcons[i],next[i]);
        }
        iconColor=background;iconDpi=dpi;
    }
    for(auto icon:next)if(icon)DestroyIcon(icon);
}
void theme(){
    HWND window=GetParent(edit);
    updateNoteIcons(window);
    COLORREF border=DWMWA_COLOR_NONE;
    if(FAILED(DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border)))){
        DWMNCRENDERINGPOLICY policy=DWMNCRP_DISABLED;
        DwmSetWindowAttribute(window,DWMWA_NCRENDERING_POLICY,&policy,sizeof(policy));
    }
    SendMessage(edit,EM_SETBKGNDCOLOR,0,background);

    RedrawWindow(window,nullptr,nullptr,RDW_FRAME|RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
}
void rememberWindow(HWND w){
    if(IsIconic(w))return;
    RECT r;GetWindowRect(w,&r);
    auto tmp=base/L"window.tmp";
    ofstream out(tmp);
    out<<r.left<<' '<<r.top<<' '<<r.right<<' '<<r.bottom<<' '
       <<((GetWindowLongPtr(w,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0);
    for(auto color:colors)out<<' '<<color;
    out<<' '<<barBrightness;
    out.close();
    if(out)MoveFileEx(tmp.c_str(),(base/L"window.settings").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
void restoreWindow(HWND w){
    ifstream in(base/L"window.settings");
    RECT r;int pinned;
    if(!(in>>r.left>>r.top>>r.right>>r.bottom>>pinned))return;
    long long width=(long long)r.right-r.left,height=(long long)r.bottom-r.top;
    if(width<=0||height<=0)return;
    MONITORINFO monitor{sizeof(monitor)};
    if(!GetMonitorInfo(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&monitor))return;
    auto area=monitor.rcWork;
    LONG cx=(LONG)min<long long>(width,area.right-area.left);
    LONG cy=(LONG)min<long long>(height,area.bottom-area.top);
    LONG x=max<LONG>(area.left,min<LONG>(r.left,area.right-cx));
    LONG y=max<LONG>(area.top,min<LONG>(r.top,area.bottom-cy));
    for(auto& color:colors){COLORREF value;if(!(in>>value))break;if(!portableColors)color=value&0xffffff;}
    int brightness;
    if(in>>brightness && !portableColors && brightness>=50 && brightness<=98)barBrightness=brightness;
    SetWindowPos(w,pinned?HWND_TOPMOST:HWND_NOTOPMOST,x,y,cx,cy,SWP_NOACTIVATE);
}
int px(HWND w,int dip){return MulDiv(dip,GetDpiForWindow(w),96);}
int dragOffset=-1;
LRESULT CALLBACK scrollProc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    RECT r;GetClientRect(w,&r);
    SCROLLINFO info{sizeof(info),SIF_ALL};
    GetScrollInfo(edit,SB_VERT,&info);
    int range=max(0,info.nMax-info.nMin+1-(int)info.nPage);
    int height=r.bottom;
    int size=range?min(height,max(px(w,22),MulDiv(height,info.nPage,range+info.nPage))):height;
    int travel=height-size;
    int top=range?MulDiv(travel,info.nPos-info.nMin,range):0;
    int y=GET_Y_LPARAM(lp);
    switch(msg){
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);
        SetDCBrushColor(dc,background);FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));
        if(range){
            SelectObject(dc,GetStockObject(NULL_PEN));SelectObject(dc,GetStockObject(DC_BRUSH));
            int pad=max(1L,r.right/4);
            SetDCBrushColor(dc,trackColor);Rectangle(dc,pad,0,r.right-pad,height);
            SetDCBrushColor(dc,thumbColor);Rectangle(dc,pad,top,r.right-pad,top+size);
        }
        EndPaint(w,&ps);return 0;
    }
    case WM_LBUTTONDOWN:
        if(!range)return 0;
        if(y<top || y>=top+size){
            SendMessage(edit,WM_VSCROLL,y<top?SB_PAGEUP:SB_PAGEDOWN,0);
        }else{dragOffset=y-top;SetCapture(w);}
        InvalidateRect(w,nullptr,FALSE);return 0;
    case WM_MOUSEMOVE:
        if(dragOffset>=0 && GetCapture()==w && travel>0){
            POINT pos{};SendMessage(edit,EM_GETSCROLLPOS,0,(LPARAM)&pos);
            int next=info.nMin+MulDiv(max(0,min(travel,y-dragOffset)),range,travel);
            pos.y+=next-info.nPos;
            SendMessage(edit,EM_SETSCROLLPOS,0,(LPARAM)&pos);
            InvalidateRect(w,nullptr,FALSE);
        }
        return 0;
    case WM_LBUTTONUP:if(GetCapture()==w)ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:dragOffset=-1;return 0;
    case WM_MOUSEWHEEL:return SendMessage(edit,msg,wp,lp);
    }
    return DefWindowProc(w,msg,wp,lp);
}
INT_PTR CALLBACK sizeDialog(HWND dialog,UINT msg,WPARAM wp,LPARAM lp){
    HWND slider=GetDlgItem(dialog,201);
    if(msg==WM_INITDIALOG){
        SendMessage(slider,TBM_SETRANGE,TRUE,MAKELPARAM(1,30));
        SendMessage(slider,TBM_SETPOS,TRUE,lp);
        SetDlgItemInt(dialog,202,(UINT)lp,FALSE);
        return TRUE;
    }
    if(msg==WM_HSCROLL){
        SetDlgItemInt(dialog,202,(UINT)SendMessage(slider,TBM_GETPOS,0,0),FALSE);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK){
        EndDialog(dialog,SendMessage(slider,TBM_GETPOS,0,0));
        return TRUE;
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL)){
        EndDialog(dialog,0);return TRUE;
    }
    return FALSE;
}
int fontLanguage(unsigned c){
    if((c>=0x3400 && c<=0x9fff)||(c>=0xf900 && c<=0xfaff)||(c>=0x20000 && c<=0x323af)||
       (c>=0x3000 && c<=0x303f)||(c>=0xff01 && c<=0xff60))return 1;
    if((c>=33 && c<=126)||(c>=0xc0 && c<=0x24f))return 0;
    return -1;
}
void applyLanguageFaces(CHARRANGE span,int only){
    if(span.cpMax<=span.cpMin)return;
    auto doc=textDocument();
    wstring text(span.cpMax-span.cpMin+1,L'\0');
    TEXTRANGE range{span,text.data()};
    int length=(int)SendMessage(edit,EM_GETTEXTRANGE,0,(LPARAM)&range);
    int first=0,previous=-1;
    auto apply=[&](int end){
        if(previous<0 || (only>=0 && previous!=only) || languageFaces[previous].empty())return;
        winrt::com_ptr<ITextRange> target;
        winrt::com_ptr<ITextFont> font;
        if(FAILED(doc->Range(span.cpMin+first,span.cpMin+end,target.put())) ||
           FAILED(target->GetFont(font.put())))return;
        BSTR name=SysAllocString(languageFaces[previous].c_str());
        if(name){font->SetName(name);SysFreeString(name);}    };
    for(int i=0;i<length;){
        int from=i;unsigned c=text[i++];
        if(IS_HIGH_SURROGATE(c) && i<length && IS_LOW_SURROGATE(text[i]))
            c=0x10000+((c-0xd800)<<10)+(text[i++]-0xdc00);
        int language=fontLanguage(c);
        if(language!=previous){apply(from);first=from;previous=language;}
    }
    apply(length);
}
void styleListMarkers(CHARRANGE scope,bool numbers=false){
    auto doc=textDocument();
    winrt::com_ptr<ITextRange> mark;
    winrt::check_hresult(doc->Range(scope.cpMin,scope.cpMin,mark.put()));
    long length;winrt::check_hresult(mark->GetStoryLength(&length));
    CHARFORMAT2 defaults{sizeof(defaults)};
    SendMessage(edit,EM_GETCHARFORMAT,SCF_DEFAULT,(LPARAM)&defaults);
    const wchar_t* face=languageFaces[0].empty()?defaults.szFaceName:languageFaces[0].c_str();
    long pos=scope.cpMin;
    do{
        long moved,end,type;
        winrt::check_hresult(mark->MoveEnd(tomParagraph,1,&moved));
        winrt::check_hresult(mark->GetEnd(&end));
        if(end<=pos)break;
        winrt::check_hresult(mark->SetRange(end-1,end));
        winrt::com_ptr<ITextPara> para;winrt::check_hresult(mark->GetPara(para.put()));
        winrt::check_hresult(para->GetListType(&type));
        if((type&0xffff)==tomListBullet && end>1){
            winrt::com_ptr<ITextRange> first;
            winrt::check_hresult(doc->Range(end-1,end-1,first.put()));
            long distance;first->StartOf(tomParagraph,tomMove,&distance);
            long start;first->GetStart(&start);
            if(start<end-1){
                first->SetEnd(start+1);
                winrt::com_ptr<ITextFont> body,marker;
                first->GetFont(body.put());mark->GetFont(marker.put());
                float size,offset;
                if(SUCCEEDED(body->GetSize(&size)))marker->SetSize(size);
                if(SUCCEEDED(body->GetPosition(&offset)))marker->SetPosition(offset);
            }
        }
        if(numbers && (type&0xffff)==tomListNumberAsArabic){
            winrt::com_ptr<ITextFont> font;winrt::check_hresult(mark->GetFont(font.put()));
            BSTR name=SysAllocString(face);
            if(name){font->SetName(name);SysFreeString(name);}
            font->SetSize(defaults.yHeight/20.f);
            font->SetBold(tomFalse);font->SetItalic(tomFalse);
            font->SetStrikeThrough(tomFalse);font->SetUnderline(tomNone);
            font->SetForeColor(defaults.dwEffects&CFE_AUTOCOLOR?tomAutoColor:(long)defaults.crTextColor);
        }
        pos=end;winrt::check_hresult(mark->SetRange(pos,pos));
    }while(pos<length && pos<scope.cpMax);
}
bool chooseLanguageFace(HWND w){
    HMENU menu=CreatePopupMenu();
    AppendMenu(menu,MF_STRING,1,L"中文字体…");AppendMenu(menu,MF_STRING,2,L"英文字体…");
    RECT r;GetWindowRect(GetDlgItem(w,100),&r);
    UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_BOTTOMALIGN,r.left,r.top,0,w,nullptr);
    DestroyMenu(menu);if(!choice)return false;
    int language=choice==1?1:0;
    LOGFONT face{};face.lfCharSet=language?GB2312_CHARSET:ANSI_CHARSET;
    lstrcpyn(face.lfFaceName,languageFaces[language].c_str(),LF_FACESIZE);
    CHOOSEFONT picker{sizeof(picker)};picker.hwndOwner=w;picker.lpLogFont=&face;
    picker.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT|CF_NOSIZESEL|CF_NOSTYLESEL;
    if(!ChooseFont(&picker))return false;
    languageFaces[language]=face.lfFaceName;
    GETTEXTLENGTHEX len{GTL_NUMCHARS|GTL_PRECISE,1200};
    CHARRANGE scope{0,(LONG)SendMessage(edit,EM_GETTEXTLENGTHEX,(WPARAM)&len,0)};
    POINT scroll{};SendMessage(edit,EM_GETSCROLLPOS,0,(LPARAM)&scroll);
    applyLanguageFaces(scope,language);
    styleListMarkers(scope,!language);
    SendMessage(edit,EM_SETSCROLLPOS,0,(LPARAM)&scroll);
    return true;
}
bool legacyComposition=false,richComposition=false;
LONG compositionStart=0;
constexpr UINT finishInput=WM_APP+12;
bool composing(){return legacyComposition || richComposition;}

ULONGLONG saveDeadline=0,syncDeadline=0;

constexpr UINT_PTR autoSyncTimer=4;
bool pendingSync=false;
void armAutoSync(HWND window){
    UINT syncDelayMs=syncDelayMinutes*60000;
    syncDeadline=GetTickCount64()+syncDelayMs;
    SetTimer(window,autoSyncTimer,syncDelayMs,nullptr);
}
void noteChanged(HWND window){
    dirty=true;
    pendingSync=true;
    if(composing())return;
    armAutoSync(window);
    UINT saveDelayMs=saveDelaySeconds*1000;
    saveDeadline=GetTickCount64()+saveDelayMs;
    SetTimer(window,1,saveDelayMs,nullptr);
}
void beginInput(bool& source){
    if(!composing()){
        CHARRANGE range{};SendMessage(edit,EM_EXGETSEL,0,(LPARAM)&range);
        compositionStart=range.cpMin;
        KillTimer(GetParent(edit),1);
        KillTimer(GetParent(edit),autoSyncTimer);
    }
    source=true;
}
void endInput(bool& source){
    if(!source)return;
    source=false;
    if(!composing()){
        CHARRANGE range{};SendMessage(edit,EM_EXGETSEL,0,(LPARAM)&range);
        if(range.cpMin==range.cpMax && range.cpMax>compositionStart)
            PostMessage(edit,finishInput,compositionStart,range.cpMax);
        if(dirty)noteChanged(GetParent(edit));
        else if(pendingSync)armAutoSync(GetParent(edit));
    }
}LRESULT CALLBACK editProc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==WM_KEYDOWN && (GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000)
        && (wp==VK_OEM_4 || wp==VK_OEM_6) && !syncJob){
        PARAFORMAT2 p{sizeof(p)};SendMessage(w,EM_GETPARAFORMAT,0,(LPARAM)&p);
        int spacing=p.bLineSpacingRule==5?p.dyLineSpacing:p.bLineSpacingRule==1?30:p.bLineSpacingRule==2?40:20;
        p.dwMask=PFM_LINESPACING;p.bLineSpacingRule=5;
        p.dyLineSpacing=max(10,min(100,spacing+(wp==VK_OEM_6?2:-2)));
        CHARRANGE selection;SendMessage(w,EM_EXGETSEL,0,(LPARAM)&selection);
        POINT scroll{};SendMessage(w,EM_GETSCROLLPOS,0,(LPARAM)&scroll);
        SendMessage(w,WM_SETREDRAW,FALSE,0);
        SendMessage(w,EM_SETSEL,0,-1);
        SendMessage(w,EM_SETPARAFORMAT,0,(LPARAM)&p);
        CHARRANGE whole;SendMessage(w,EM_EXGETSEL,0,(LPARAM)&whole);
        styleListMarkers(whole);
        SendMessage(w,EM_EXSETSEL,0,(LPARAM)&selection);
        SendMessage(w,EM_SETSCROLLPOS,0,(LPARAM)&scroll);
        SendMessage(w,WM_SETREDRAW,TRUE,0);InvalidateRect(w,nullptr,TRUE);
        noteChanged(GetParent(w));return 0;
    }
    if(msg==WM_CHAR && (GetKeyState(VK_CONTROL)&0x8000) && (wp==27 || wp==29))return 0;    if(msg==WM_IME_STARTCOMPOSITION)beginInput(legacyComposition);
    if(msg==finishInput){
        if(!composing() && (!languageFaces[0].empty() || !languageFaces[1].empty()))
            applyLanguageFaces({(LONG)wp,(LONG)lp},-1);
        return 0;
    }
    bool input=!composing() && msg==WM_CHAR && wp>=32 && !(GetKeyState(VK_CONTROL)&0x8000);    bool customFonts=!languageFaces[0].empty() || !languageFaces[1].empty();
    CHARRANGE before{};
    if(input && customFonts)SendMessage(w,EM_EXGETSEL,0,(LPARAM)&before);
    auto result=DefSubclassProc(w,msg,wp,lp);
    if(msg==WM_IME_ENDCOMPOSITION)endInput(legacyComposition);
    if(customFonts && input && !composing()){
        CHARRANGE after{};SendMessage(w,EM_EXGETSEL,0,(LPARAM)&after);
        if(after.cpMin==after.cpMax)
            applyLanguageFaces({before.cpMin,after.cpMax},-1);
    }    if(msg==WM_PAINT && bar){
        bool needed=(GetWindowLongPtr(w,GWL_STYLE)&WS_VSCROLL)!=0;
        bool shown=(GetWindowLongPtr(bar,GWL_STYLE)&WS_VISIBLE)!=0;
        if(needed!=shown)ShowWindow(bar,needed?SW_SHOWNOACTIVATE:SW_HIDE);
        if(needed)InvalidateRect(bar,nullptr,FALSE);
    }
    return result;
}
void layout(HWND w){
    RECT r;GetClientRect(w,&r);
    int top=r.bottom-px(w,32);
    MoveWindow(syncNotice,r.right-px(w,106),top-px(w,34),px(w,96),px(w,28),TRUE);
    for(int i=0;i<9;++i){
        int step=min<int>(px(w,30),(r.right-2*px(w,6))/9);
        int slot=i==8?3:i>=3?i+1:i;
        int x=slot>=7?r.right-px(w,6)-(9-slot)*step:px(w,6)+slot*step;
        int buttonHeight=px(w,24);
        int buttonTop=top+(r.bottom-top-buttonHeight)/2;
        MoveWindow(GetDlgItem(w,100+i),x,buttonTop,min<int>(step,px(w,28)),buttonHeight,TRUE);
    }
    MoveWindow(edit,px(w,8),px(w,10),r.right-px(w,16),top-px(w,12),TRUE);
    if(bar){
        RECT client;GetClientRect(edit,&client);
        int strip=GetSystemMetricsForDpi(SM_CXVSCROLL,GetDpiForWindow(w));
        SetWindowPos(bar,HWND_TOP,r.right-px(w,8)-strip,px(w,10),strip,client.bottom,SWP_NOACTIVATE);
    }
}void updateFont(HWND w){
    if(scriptFont)DeleteObject(scriptFont);
    scriptFont=CreateFont(-px(w,19),0,0,0,FW_NORMAL,TRUE,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Script");
    HFONT previous=ui;
    ui=CreateFont(-px(w,14),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe MDL2 Assets");
    for(int id=100;id<=108;++id)SendDlgItemMessage(w,id,WM_SETFONT,(WPARAM)ui,TRUE);
    if(previous)DeleteObject(previous);
}
int hitArea(HWND w,POINT p){

    RECT r;GetClientRect(w,&r);int edge=px(w,6);
    bool left=p.x<edge,right=p.x>=r.right-edge;
    int corner=px(w,20);
    if(p.y>=r.bottom-corner && p.x<corner)return HTBOTTOMLEFT;
    if(p.y>=r.bottom-corner && p.x>=r.right-corner)return HTBOTTOMRIGHT;
    if(p.y<edge)return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
    if(p.y>=r.bottom-edge)return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
    if(left||right)return left?HTLEFT:HTRIGHT;
    if(ChildWindowFromPoint(w,p)!=w)return HTCLIENT;return HTCAPTION;
}
RECT resizeStart{}; POINT dragPoint{}; int resizeEdge=0;
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM k,LPARAM l){switch(m){
case WM_CREATE:{const wchar_t* names[]={L"字体 / 字号",L"粗体",L"斜体",L"选区字号",L"列表样式",L"配色",L"同步",L"窗口置顶",L"删除线"};int x=8;for(int i=0;i<9;i++){int width=i==0?100:48;auto h=CreateWindow(L"BUTTON",names[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,x,8,width,32,w,(HMENU)(INT_PTR)(100+i),0,0);SendMessage(h,WM_SETFONT,(WPARAM)ui,1);x+=width+4;}syncNotice=CreateWindow(L"STATIC",L"",WS_CHILD|SS_OWNERDRAW,0,0,0,0,w,(HMENU)210,nullptr,nullptr);updateFont(w);edit=CreateWindowEx(0,MSFTEDIT_CLASS,L"",WS_CHILD|WS_VISIBLE|ES_NOHIDESEL|ES_MULTILINE|WS_CLIPSIBLINGS|WS_VSCROLL|ES_AUTOVSCROLL|ES_WANTRETURN,0,0,0,0,w,(HMENU)1,0,0);CHARFORMAT2 initial{sizeof(initial)};initial.dwMask=CFM_SIZE;initial.yHeight=240;SendMessage(edit,EM_SETCHARFORMAT,SCF_DEFAULT,(LPARAM)&initial);deriveScrollColors();if(!edit||!load()){MessageBox(w,L"无法读取便笺，已保留原文件。",L"Sticky Notes",MB_OK);return -1;}WNDCLASS sc{};sc.lpfnWndProc=scrollProc;sc.hInstance=GetModuleHandle(nullptr);sc.hCursor=LoadCursor(nullptr,IDC_ARROW);sc.lpszClassName=L"NoteScrollStrip";RegisterClass(&sc);bar=CreateWindow(sc.lpszClassName,L"",WS_CHILD|WS_VISIBLE,0,0,0,0,w,nullptr,sc.hInstance,nullptr);SetWindowSubclass(edit,editProc,1,0);theme();SendMessage(edit,EM_SETEVENTMASK,0,ENM_CHANGE|ENM_STARTCOMPOSITION|ENM_ENDCOMPOSITION);loading=false;SetFocus(edit);return 0;}
case WM_ERASEBKGND:{
    RECT r;GetClientRect(w,&r);
    HDC dc=(HDC)k;SetDCBrushColor(dc,background);
    FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));
    r.top=r.bottom-px(w,32);
    SetDCBrushColor(dc,barColor());
    FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));return 1;
}
case WM_DRAWITEM:{
    auto item=(DRAWITEMSTRUCT*)l;
    if(item->CtlID==210){
        HDC dc=item->hDC;int saved=SaveDC(dc);
        RECT r=item->rcItem;
        SetDCBrushColor(dc,background);FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));
        SelectObject(dc,GetStockObject(NULL_PEN));SelectObject(dc,GetStockObject(DC_BRUSH));
        COLORREF surface=lastSyncSucceeded?RGB(35,79,62):RGB(116,49,48);
        SetDCBrushColor(dc,surface);RoundRect(dc,0,0,r.right,r.bottom,px(w,12),px(w,12));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        RECT icon=r;icon.left=px(w,7);icon.right=px(w,26);
        SelectObject(dc,ui);
        DrawText(dc,lastSyncSucceeded?L"\xE73E":L"\xE783",1,&icon,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
        NONCLIENTMETRICS metrics{sizeof(metrics)};
        HFONT label=nullptr;
        if(SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,GetDpiForWindow(w)))
            label=CreateFontIndirect(&metrics.lfMessageFont);
        SelectObject(dc,label?(HGDIOBJ)label:GetStockObject(DEFAULT_GUI_FONT));
        r.left=px(w,29);
        DrawText(dc,lastSyncSucceeded?L"同步成功":L"同步失败",-1,&r,DT_SINGLELINE|DT_VCENTER);
        RestoreDC(dc,saved);if(label)DeleteObject(label);return TRUE;
    }
    if(item->CtlID<100||item->CtlID>108)break;
    const wchar_t glyphs[]={0xE8D2,0xE8DD,0xE8DB,0xE8E9,0xE8FD,0xE790,0xE895,0xE718,0x0053};
    HDC dc=item->hDC;int saved=SaveDC(dc);
    RECT r=item->rcItem;
    COLORREF surface=barColor();

    int luminance=299*GetRValue(surface)+587*GetGValue(surface)+114*GetBValue(surface);
    COLORREF ink=luminance>140000?RGB(32,32,32):RGB(255,255,255);
    if(item->CtlID==106 && syncJob && iconGraphics){
        using namespace Gdiplus;
        int width=r.right-r.left,height=r.bottom-r.top;
        if(!syncFrame || syncFrame->GetWidth()!=(UINT)width || syncFrame->GetHeight()!=(UINT)height)
            syncFrame=std::make_unique<Bitmap>(width,height,PixelFormat32bppPARGB);
        Bitmap& frame=*syncFrame;
        {
            Graphics canvas(&frame);
            canvas.Clear(Color(255,GetRValue(surface),GetGValue(surface),GetBValue(surface)));
            canvas.SetSmoothingMode(SmoothingModeAntiAlias);
            float diameter=(float)px(w,12);
            RectF ring((width-diameter)/2.f,(height-diameter)/2.f,diameter,diameter);
            Pen pen(Color(255,GetRValue(ink),GetGValue(ink),GetBValue(ink)),diameter/7.f);
            pen.SetLineCap(LineCapRound,LineCapRound,DashCapRound);
            float angle=(float)((GetTickCount64()-syncAnimationStart)%1400)*360.f/1400.f;
            canvas.DrawArc(&pen,ring,angle,120.f);
            canvas.DrawArc(&pen,ring,angle+180.f,120.f);
        }
        Graphics target(dc);
        target.DrawImage(&frame,r.left,r.top,0,0,width,height,UnitPixel);
        RestoreDC(dc,saved);return TRUE;
    }
    SetDCBrushColor(dc,surface);FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));    SetTextColor(dc,ink);SetBkMode(dc,TRANSPARENT);SelectObject(dc,ui);
    if(item->itemState&ODS_SELECTED)OffsetRect(&r,0,px(w,1));
    if(item->CtlID==100)SelectObject(dc,scriptFont);
    if(item->CtlID==108){
        SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
        RECT line={r.left+(r.right-r.left)/2-px(w,6),(r.top+r.bottom)/2,r.left+(r.right-r.left)/2+px(w,6),(r.top+r.bottom)/2+px(w,1)};
        SetDCBrushColor(dc,ink);FillRect(dc,&line,(HBRUSH)GetStockObject(DC_BRUSH));
    }
    DrawText(dc,item->CtlID==100?L"f":&glyphs[item->CtlID-100],1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(item->CtlID==107 && (GetWindowLongPtr(w,GWL_EXSTYLE)&WS_EX_TOPMOST)){
        RECT indicator={r.left+px(w,9),r.bottom-px(w,5),r.right-px(w,9),r.bottom-px(w,3)};
        SetDCBrushColor(dc,ink);FillRect(dc,&indicator,(HBRUSH)GetStockObject(DC_BRUSH));
    }
    if((item->itemState&ODS_FOCUS) && !(item->itemState&ODS_NOFOCUSRECT) && !(item->CtlID==106 && syncJob))DrawFocusRect(dc,&r);
    RestoreDC(dc,saved);return TRUE;
}
case WM_NCCALCSIZE:
case WM_NCPAINT:return 0;
case WM_NCACTIVATE:return TRUE;
case WM_LBUTTONDOWN:{
    POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
    int hit=hitArea(w,p);
    if(hit<HTLEFT || hit>HTBOTTOMRIGHT)break;
    GetWindowRect(w,&resizeStart);
    dragPoint=p;ClientToScreen(w,&dragPoint);
    resizeEdge=hit;
    SetCapture(w);
    return 0;
}
case WM_MOUSEMOVE:{
    if(!resizeEdge || GetCapture()!=w)break;
    POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(w,&p);
    RECT r=resizeStart;
    LONG dx=p.x-dragPoint.x,dy=p.y-dragPoint.y;
    int edge=resizeEdge;
    if(edge==HTLEFT || edge==HTTOPLEFT || edge==HTBOTTOMLEFT)
        r.left=min<LONG>(r.left+dx,r.right-px(w,224));
    if(edge==HTRIGHT || edge==HTTOPRIGHT || edge==HTBOTTOMRIGHT)
        r.right=max<LONG>(r.right+dx,r.left+px(w,224));
    if(edge==HTTOP || edge==HTTOPLEFT || edge==HTTOPRIGHT)
        r.top=min<LONG>(r.top+dy,r.bottom-px(w,108));
    if(edge==HTBOTTOM || edge==HTBOTTOMLEFT || edge==HTBOTTOMRIGHT)
        r.bottom=max<LONG>(r.bottom+dy,r.top+px(w,108));
    SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
    return 0;
}
case WM_LBUTTONUP:
case WM_CANCELMODE:
    if(!resizeEdge)break;
    ReleaseCapture();return 0;
case WM_CAPTURECHANGED:
    if(resizeEdge){resizeEdge=0;rememberWindow(w);}
    return 0;
case WM_EXITSIZEMOVE:rememberWindow(w);return 0;
case WM_SIZE:layout(w);InvalidateRect(w,nullptr,TRUE);return 0;
case WM_CONTEXTMENU:{
    if((HWND)k!=GetDlgItem(w,105))break;
    if(syncJob)return 0;
    HMENU menu=CreatePopupMenu();
    const int levels[]={95,86,72,55};
    for(int i=0;i<4;++i){
        wstring label=L"底栏亮度 "+to_wstring(levels[i])+L"%";
        AppendMenu(menu,MF_STRING|(barBrightness==levels[i]?MF_CHECKED:0),i+1,label.c_str());
    }
    POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
    if(p.x==-1 && p.y==-1){RECT r;GetWindowRect((HWND)k,&r);p={r.left,r.top};}
    UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,w,nullptr);
    DestroyMenu(menu);
    if(choice>=1 && choice<=4){barBrightness=levels[choice-1];noteChanged(w);rememberWindow(w);theme();}
    return 0;
}
case WM_GETMINMAXINFO:((MINMAXINFO*)l)->ptMinTrackSize={px(w,224),px(w,108)};return 0;
case WM_DPICHANGED:{
    RECT r=*(RECT*)l;
    SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
    updateFont(w);layout(w);updateNoteIcons(w);return 0;
}
case WM_NCHITTEST:{
    POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(w,&p);
    int hit=hitArea(w,p);
    return hit==HTCAPTION?HTCAPTION:HTCLIENT;
}
case WM_SETCURSOR:{
    if(LOWORD(l)!=HTCLIENT || (HWND)k!=w)break;
    POINT p;GetCursorPos(&p);ScreenToClient(w,&p);
    LPCWSTR cursor=nullptr;
    switch(hitArea(w,p)){
    case HTBOTTOMLEFT:case HTTOPRIGHT:cursor=IDC_SIZENESW;break;
    case HTBOTTOMRIGHT:case HTTOPLEFT:cursor=IDC_SIZENWSE;break;
    case HTLEFT:case HTRIGHT:cursor=IDC_SIZEWE;break;
    case HTTOP:case HTBOTTOM:cursor=IDC_SIZENS;break;
    }
    if(!cursor)break;
    SetCursor(LoadCursor(nullptr,cursor));return TRUE;
}case WM_NOTIFY:{
    auto header=(NMHDR*)l;
    if(header && header->hwndFrom==edit){
        if(header->code==EN_STARTCOMPOSITION)beginInput(richComposition);
        if(header->code==EN_ENDCOMPOSITION)endInput(richComposition);
    }
    break;
}
case WM_COMMAND:{int id=LOWORD(k);if(id==1&&HIWORD(k)==EN_CHANGE&&!loading){noteChanged(w);}if(id==107){bool pinned=(GetWindowLongPtr(w,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;SetWindowPos(w,pinned?HWND_NOTOPMOST:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);rememberWindow(w);InvalidateRect(GetDlgItem(w,107),nullptr,FALSE);SetFocus(edit);return 0;}if(id<100||id>108||syncJob)return 0;CHARFORMAT2 cf{};cf.cbSize=sizeof(cf);SendMessage(edit,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);if(id==100 && !chooseLanguageFace(w)){SetFocus(edit);return 0;}if(id==101||id==102||id==108){cf.dwMask=id==108?CFM_STRIKEOUT:id==101?CFM_BOLD:CFM_ITALIC;cf.dwEffects^=cf.dwMask;}if(id==101||id==102||id==108)SendMessage(edit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);if(id==103){
    auto size=DialogBoxParam(GetModuleHandle(nullptr),MAKEINTRESOURCE(200),w,sizeDialog,(cf.dwMask&CFM_SIZE)?max(1L,min(30L,(cf.yHeight+10)/20)):12);
    SetFocus(edit);if(size<1)return 0;
    cf.dwMask=CFM_SIZE;cf.yHeight=(LONG)size*20;
    SendMessage(edit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
    CHARRANGE affected;SendMessage(edit,EM_EXGETSEL,0,(LPARAM)&affected);styleListMarkers(affected);
}
if(id==104){
    HMENU menu=CreatePopupMenu();
    PARAFORMAT2 paragraph{sizeof(PARAFORMAT2)};
    {        SendMessage(edit,EM_GETPARAFORMAT,0,(LPARAM)&paragraph);
        const wchar_t* labels[]={L"普通段落",L"无序列表",L"有序列表"};
        const WORD styles[]={0,PFN_BULLET,PFN_ARABIC};
        for(int i=0;i<3;++i){
            bool checked=(paragraph.dwMask&PFM_NUMBERING) && paragraph.wNumbering==styles[i];
            AppendMenu(menu,MF_STRING|(checked?MF_CHECKED:0),i+1,labels[i]);
        }
    }
    RECT button;GetWindowRect(GetDlgItem(w,id),&button);
    UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_BOTTOMALIGN,button.left,button.top,0,w,nullptr);
    DestroyMenu(menu);SetFocus(edit);
    if(!choice)return 0;
    {
        paragraph.dwMask=PFM_NUMBERING|PFM_NUMBERINGSTART;
        paragraph.wNumbering=choice==3?PFN_ARABIC:choice==2?PFN_BULLET:0;
        paragraph.wNumberingStart=1;
        if(choice==3){paragraph.dwMask|=PFM_NUMBERINGSTYLE;paragraph.wNumberingStyle=PFNS_PAREN;}
        SendMessage(edit,EM_SETPARAFORMAT,0,(LPARAM)&paragraph);
        if(choice>=2){CHARRANGE scope;SendMessage(edit,EM_EXGETSEL,0,(LPARAM)&scope);styleListMarkers(scope,choice==3);}
    }
}if(id==105){
    CHARRANGE selection;SendMessage(edit,EM_EXGETSEL,0,(LPARAM)&selection);
    bool textSelected=selection.cpMax>selection.cpMin;
    CHOOSECOLOR picker{sizeof(picker)};picker.hwndOwner=w;picker.lpCustColors=colors;
    picker.rgbResult=textSelected?((cf.dwEffects&CFE_AUTOCOLOR)?GetSysColor(COLOR_WINDOWTEXT):cf.crTextColor):background;
    picker.Flags=CC_FULLOPEN|CC_RGBINIT;
    bool chosen=ChooseColor(&picker)!=FALSE;
    if(chosen){
        auto existing=find(begin(colors),end(colors),picker.rgbResult);
        auto insertEnd=existing==end(colors)?end(colors)-1:existing;
        move_backward(begin(colors),insertEnd,insertEnd+1);
        colors[0]=picker.rgbResult;
        if(textSelected){
            cf.dwMask=CFM_COLOR;cf.dwEffects=0;cf.crTextColor=picker.rgbResult;
            SendMessage(edit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
        }else{background=picker.rgbResult;deriveScrollColors();theme();}
    }
    rememberWindow(w);noteChanged(w);SetFocus(edit);return 0;
}if(id==106){KillTimer(w,autoSyncTimer);if(!save()){showSyncResult(w,false);return 0;}ShowWindow(syncNotice,SW_HIDE);wstring cmd=L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \""+(base/L"sync.ps1").wstring()+L"\"";STARTUPINFO si{sizeof(si)};PROCESS_INFORMATION pi{};if(CreateProcess(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,base.c_str(),&si,&pi)){CloseHandle(pi.hThread);syncAnimationStart=GetTickCount64();syncJob=pi.hProcess;SendMessage(edit,EM_SETREADONLY,TRUE,0);SetWindowText(w,L"正在同步…");SetTimer(w,2,25,0);}else showSyncResult(w,false);return 0;}noteChanged(w);SetFocus(edit);return 0;}
case WM_TIMER:
    if(k==autoSyncTimer){
        if(!pendingSync || composing() || syncJob || GetTickCount64()<syncDeadline)return 0;
        KillTimer(w,autoSyncTimer);
        SendMessage(w,WM_COMMAND,106,0);
        return 0;
    }
    if(k==3){KillTimer(w,3);ShowWindow(syncNotice,SW_HIDE);return 0;}if(k==2)InvalidateRect(GetDlgItem(w,106),nullptr,FALSE);if(k==1){if(composing() || GetTickCount64()<saveDeadline)return 0;KillTimer(w,1);if(!save())MessageBox(w,L"无法保存便笺，请检查磁盘空间和目录权限。",L"保存失败",MB_ICONERROR);}else if(syncJob&&WaitForSingleObject(syncJob,0)==0){DWORD rc;GetExitCodeProcess(syncJob,&rc);CloseHandle(syncJob);syncJob=nullptr;syncFrame.reset();KillTimer(w,2);loading=true;if(rc==10){rc=load()?0:1;}loading=false;theme();SendMessage(edit,EM_SETREADONLY,FALSE,0);SetWindowText(w,L"Sticky Notes");if(rc==0){pendingSync=false;std::error_code ec;fs::remove(base/L".sync-pending",ec);}
    showSyncResult(w,rc==0);}return 0;
case WM_CLOSE:if(syncJob)return 0;if(!save()){MessageBox(w,L"便笺尚未保存，请检查磁盘空间和目录权限。",L"保存失败",MB_ICONERROR);return 0;}if(pendingSync){ofstream marker(base/L".sync-pending");marker<<"1";}rememberWindow(w);break;
case WM_DESTROY:DeleteObject(scriptFont);DeleteObject(ui);PostQuitMessage(0);return 0;}return DefWindowProc(w,m,k,l);}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR command,int show){winrt::init_apartment(winrt::apartment_type::single_threaded);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};InitCommonControlsEx(&controls);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);if(wcscmp(command,L"--validate-note")==0){
        LoadLibrary(L"Msftedit.dll");
        edit=CreateWindow(MSFTEDIT_CLASS,L"",WS_POPUP|ES_MULTILINE,0,0,400,200,nullptr,nullptr,h,nullptr);
        file=fs::current_path()/L"notes.json";
        return edit && fs::exists(file) && load()?0:1;
    }
    HANDLE mutex=CreateMutex(nullptr,FALSE,L"Local\\MyStickyNotesDesktop");if(GetLastError()==ERROR_ALREADY_EXISTS)return 0;wchar_t path[32768];GetModuleFileName(nullptr,path,32768);base=fs::path(path).parent_path();file=base/L"notes.json";LoadLibrary(L"Msftedit.dll");WNDCLASS c{};c.hIcon=LoadIcon(h,MAKEINTRESOURCE(1));c.hInstance=h;c.lpfnWndProc=proc;c.lpszClassName=L"MyStickyNotes";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClass(&c);auto w=CreateWindow(c.lpszClassName,L"Sticky Notes",WS_POPUP|WS_SYSMENU,80,80,MulDiv(530,GetDpiForSystem(),96),MulDiv(410,GetDpiForSystem(),96),0,0,h,0);if(!w)return 1;restoreWindow(w);if(fs::exists(base/L".sync-pending")){pendingSync=true;armAutoSync(w);}if(migrateLegacy){noteChanged(w);}ShowWindow(w,show);MSG msg;while(GetMessage(&msg,0,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}for(auto icon:themedIcons)if(icon)DestroyIcon(icon);if(iconGraphics)Gdiplus::GdiplusShutdown(iconGraphics);CloseHandle(mutex);return 0;}







