#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

using namespace std;

static constexpr int INF = 32000;
static constexpr int MATE = 30000;
static constexpr int MAX_PLY = 128;

enum Piece : int { EMPTY=0, WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6,
                   BP=-1, BN=-2, BB=-3, BR=-4, BQ=-5, BK=-6 };

static inline bool white(int p){ return p>0; }
static inline bool black(int p){ return p<0; }
static inline int absPiece(int p){ return p<0?-p:p; }
static inline int fileOf(int s){ return s&7; }
static inline int rankOf(int s){ return s>>3; }
static inline int sq(int f,int r){ return r*8+f; }

struct Move {
    uint8_t from=0,to=0,piece=0,capture=0,promo=0,flags=0;
    int score=0;
    bool operator==(const Move& o) const {
        return from==o.from && to==o.to && promo==o.promo;
    }
};

static constexpr uint8_t EP=1, CASTLE=2, DOUBLE_PUSH=4;

struct State {
    int captured=0;
    int oldEp=-1;
    int oldCastle=0;
    int oldHalfmove=0;
    int oldFullmove=1;
};

struct Board {
    array<int,64> b{};
    bool whiteTurn=true;
    int ep=-1;
    int castle=15; // WK WQ BK BQ
    int halfmove=0;
    int fullmove=1;
    uint64_t key=0;

    void clear(){ b.fill(0); whiteTurn=true; ep=-1; castle=15; halfmove=0; fullmove=1; key=0; }

    static Board start(){
        Board x; x.clear();
        const string back="RNBQKBNR";
        for(int f=0;f<8;f++){
            x.b[sq(f,0)] = back[f]=='R'?WR:back[f]=='N'?WN:back[f]=='B'?WB:back[f]=='Q'?WQ:back[f]=='K'?WK:0;
            x.b[sq(f,1)] = WP;
            x.b[sq(f,6)] = BP;
            x.b[sq(f,7)] = -(back[f]=='R'?WR:back[f]=='N'?WN:back[f]=='B'?WB:back[f]=='Q'?WQ:back[f]=='K'?WK:0);
        }
        x.rehash(); return x;
    }

    bool fromFEN(const string& fen){
        clear(); stringstream ss(fen); string p,stm,cas,eps; int hm=0,fm=1;
        if(!(ss>>p>>stm>>cas>>eps>>hm>>fm)) return false;
        int r=7,f=0;
        for(char c:p){
            if(c=='/') { if(f!=8) return false; f=0; --r; continue; }
            if(isdigit((unsigned char)c)){ f += c-'0'; }
            else {
                if(r<0||f>7) return false;
                int q=0;
                switch(c){case 'P':q=WP;break;case 'N':q=WN;break;case 'B':q=WB;break;case 'R':q=WR;break;case 'Q':q=WQ;break;case 'K':q=WK;break;
                case 'p':q=BP;break;case 'n':q=BN;break;case 'b':q=BB;break;case 'r':q=BR;break;case 'q':q=BQ;break;case 'k':q=BK;break;default:return false;}
                b[sq(f,r)]=q; ++f;
            }
        }
        if(r!=0 || f!=8) return false;
        whiteTurn=(stm!="b"); castle=0;
        if(cas.find('K')!=string::npos) castle|=1;
        if(cas.find('Q')!=string::npos) castle|=2;
        if(cas.find('k')!=string::npos) castle|=4;
        if(cas.find('q')!=string::npos) castle|=8;
        ep=-1; if(eps!="-"){ if(eps.size()!=2)return false; int ef=eps[0]-'a',er=eps[1]-'1'; if(ef<0||ef>7||er<0||er>7)return false; ep=sq(ef,er); }
        halfmove=max(0,hm); fullmove=max(1,fm); rehash(); return true;
    }

    string fen() const {
        string s;
        for(int r=7;r>=0;r--){ int empty=0; for(int f=0;f<8;f++){int p=b[sq(f,r)]; if(!p){empty++;continue;} if(empty){s+=char('0'+empty);empty=0;} const char* W=" PNBRQK"; const char* L=" pnbrqk"; s += white(p)?W[p]:L[-p];} if(empty)s+=char('0'+empty); if(r)s+='/'; }
        s += whiteTurn?" w ":" b "; string c; if(castle&1)c+='K';if(castle&2)c+='Q';if(castle&4)c+='k';if(castle&8)c+='q'; if(c.empty())c="-"; s+=c+' '; if(ep>=0){s+=char('a'+fileOf(ep));s+=char('1'+rankOf(ep));}else s+="-"; s+=' '+to_string(halfmove)+' '+to_string(fullmove); return s;
    }

    uint64_t hash64() const {
        uint64_t h=0x9e3779b97f4a7c15ULL;
        auto mix=[&](uint64_t v){h^=v+0x9e3779b97f4a7c15ULL+(h<<6)+(h>>2);};
        for(int i=0;i<64;i++) if(b[i]) mix((uint64_t)(b[i]+7)*0x9e3779b97f4a7c15ULL ^ (uint64_t)(i+1)*0xbf58476d1ce4e5b9ULL);
        mix((uint64_t)whiteTurn); mix((uint64_t)castle+17); mix((uint64_t)(ep+2)*0x94d049bb133111ebULL); return h;
    }
    void rehash(){key=hash64();}

    bool attacked(int s,bool byWhite) const {
        int f=fileOf(s),r=rankOf(s);
        int pawn=byWhite?WP:BP; int pr=byWhite?r-1:r+1;
        if(pr>=0&&pr<8){ if(f>0&&b[sq(f-1,pr)]==pawn)return true; if(f<7&&b[sq(f+1,pr)]==pawn)return true; }
        static const int kdf[8]={1,2,2,1,-1,-2,-2,-1}; static const int kdr[8]={2,1,-1,-2,-2,-1,1,2};
        int kn=byWhite?WN:BN; for(int i=0;i<8;i++){int nf=f+kdf[i],nr=r+kdr[i];if(nf>=0&&nf<8&&nr>=0&&nr<8&&b[sq(nf,nr)]==kn)return true;}
        static const int df[4]={1,1,-1,-1},dr[4]={1,-1,1,-1}; int bishop=byWhite?WB:BB,queen=byWhite?WQ:BQ;
        for(int d=0;d<4;d++){int nf=f+df[d],nr=r+dr[d];while(nf>=0&&nf<8&&nr>=0&&nr<8){int p=b[sq(nf,nr)];if(p){if(p==bishop||p==queen)return true;break;}nf+=df[d];nr+=dr[d];}}
        static const int rf[4]={1,-1,0,0},rr[4]={0,0,1,-1}; int rook=byWhite?WR:BR;
        for(int d=0;d<4;d++){int nf=f+rf[d],nr=r+rr[d];while(nf>=0&&nf<8&&nr>=0&&nr<8){int p=b[sq(nf,nr)];if(p){if(p==rook||p==queen)return true;break;}nf+=rf[d];nr+=rr[d];}}
        int king=byWhite?WK:BK; for(int df0=-1;df0<=1;df0++)for(int dr0=-1;dr0<=1;dr0++)if(df0||dr0){int nf=f+df0,nr=r+dr0;if(nf>=0&&nf<8&&nr>=0&&nr<8&&b[sq(nf,nr)]==king)return true;}
        return false;
    }

    bool inCheck(bool side) const { int k=side?WK:BK; for(int i=0;i<64;i++)if(b[i]==k)return attacked(i,!side); return true; }

    void add(vector<Move>& mv,int from,int to,int promo=0,uint8_t flags=0) const {
        Move m; m.from=from;m.to=to;m.piece=(uint8_t)b[from];m.capture=(uint8_t)b[to];m.promo=(uint8_t)promo;m.flags=flags; mv.push_back(m);
    }

    vector<Move> pseudo(bool capturesOnly=false) const {
        vector<Move> mv; mv.reserve(64); bool side=whiteTurn;
        for(int from=0;from<64;from++){
            int p=b[from]; if(!p || white(p)!=side)continue; int f=fileOf(from),r=rankOf(from),ap=absPiece(p);
            if(ap==1){
                int dir=side?1:-1, start=side?1:6, promoR=side?7:0, nr=r+dir;
                if(!capturesOnly && nr>=0&&nr<8&&b[sq(f,nr)]==0){
                    if(nr==promoR){add(mv,from,sq(f,nr),side?WQ:BQ);add(mv,from,sq(f,nr),side?WR:BR);add(mv,from,sq(f,nr),side?WB:BB);add(mv,from,sq(f,nr),side?WN:BN);} else {add(mv,from,sq(f,nr)); if(r==start&&b[sq(f,r+2*dir)]==0)add(mv,from,sq(f,r+2*dir),0,DOUBLE_PUSH);}
                }
                for(int df:{-1,1}){int nf=f+df; if(nf<0||nf>7||nr<0||nr>7)continue;int to=sq(nf,nr);if(b[to]&&white(b[to])!=side){if(nr==promoR){add(mv,from,to,side?WQ:BQ);add(mv,from,to,side?WR:BR);add(mv,from,to,side?WB:BB);add(mv,from,to,side?WN:BN);}else add(mv,from,to);}else if(to==ep)add(mv,from,to,0,EP);}
            } else if(ap==2){ static const int df[8]={1,2,2,1,-1,-2,-2,-1},dr[8]={2,1,-1,-2,-2,-1,1,2};for(int i=0;i<8;i++){int nf=f+df[i],nr=r+dr[i];if(nf<0||nf>7||nr<0||nr>7)continue;int to=sq(nf,nr);if(!b[to]||white(b[to])!=side)if(!capturesOnly||b[to])add(mv,from,to);}
            } else if(ap==3||ap==4||ap==5){ static const int bdf[4]={1,1,-1,-1},bdr[4]={1,-1,1,-1};static const int rdf[4]={1,-1,0,0},rdr[4]={0,0,1,-1}; int begin=ap==3?0:ap==4?4:0,end=ap==3?4:ap==4?8:8; for(int d=begin;d<end;d++){int nf=f+(d<4?bdf[d]:rdf[d-4]),nr=r+(d<4?bdr[d]:rdr[d-4]);while(nf>=0&&nf<8&&nr>=0&&nr<8){int to=sq(nf,nr);if(!b[to]){if(!capturesOnly)add(mv,from,to);}else{if(white(b[to])!=side)add(mv,from,to);break;}nf+=(d<4?bdf[d]:rdf[d-4]);nr+=(d<4?bdr[d]:rdr[d-4]);}}
            } else if(ap==6){for(int df=-1;df<=1;df++)for(int dr=-1;dr<=1;dr++)if(df||dr){int nf=f+df,nr=r+dr;if(nf<0||nf>7||nr<0||nr>7)continue;int to=sq(nf,nr);if((!b[to]||white(b[to])!=side)&&(!capturesOnly||b[to]))add(mv,from,to);} if(!capturesOnly){if(side&&from==4&&!inCheck(true)){if((castle&1)&&b[5]==0&&b[6]==0&&!attacked(5,false)&&!attacked(6,false))add(mv,4,6,0,CASTLE);if((castle&2)&&b[1]==0&&b[2]==0&&b[3]==0&&!attacked(3,false)&&!attacked(2,false))add(mv,4,2,0,CASTLE);} if(!side&&from==60&&!inCheck(false)){if((castle&4)&&b[61]==0&&b[62]==0&&!attacked(61,true)&&!attacked(62,true))add(mv,60,62,0,CASTLE);if((castle&8)&&b[57]==0&&b[58]==0&&b[59]==0&&!attacked(59,true)&&!attacked(58,true))add(mv,60,58,0,CASTLE);}}
            }
        }
        return mv;
    }

    bool make(const Move& m,State& st){
        st.captured=b[m.to];st.oldEp=ep;st.oldCastle=castle;st.oldHalfmove=halfmove;st.oldFullmove=fullmove;
        int p=b[m.from]; b[m.to]=p;b[m.from]=0;
        if(m.flags&EP){int cs=whiteTurn?m.to-8:m.to+8;st.captured=b[cs];b[cs]=0;}
        if(m.promo)b[m.to]=whiteTurn?m.promo:-m.promo;
        if(m.flags&CASTLE){if(m.to==6){b[5]=b[7];b[7]=0;}else if(m.to==2){b[3]=b[0];b[0]=0;}else if(m.to==62){b[61]=b[63];b[63]=0;}else if(m.to==58){b[59]=b[56];b[56]=0;}}
        ep=-1;if(m.flags&DOUBLE_PUSH)ep=whiteTurn?m.from+8:m.from-8;
        if(absPiece(p)==1||st.captured)halfmove=0;else halfmove++; if(!whiteTurn)fullmove++;
        if(p==WK)castle&=~3;if(p==BK)castle&=~12;if(m.from==0||m.to==0)castle&=~2;if(m.from==7||m.to==7)castle&=~1;if(m.from==56||m.to==56)castle&=~8;if(m.from==63||m.to==63)castle&=~4;
        whiteTurn=!whiteTurn; rehash(); return true;
    }
    void undo(const Move& m,const State& st){
        whiteTurn=!whiteTurn; if(!whiteTurn)fullmove--; halfmove=st.oldHalfmove;fullmove=st.oldFullmove;ep=st.oldEp;castle=st.oldCastle;
        int p=b[m.to]; if(m.promo)p=whiteTurn?WP:BP; b[m.from]=p;b[m.to]=st.captured;
        if(m.flags&EP){int cs=whiteTurn?m.to-8:m.to+8;b[cs]=whiteTurn?BP:WP;}
        if(m.flags&CASTLE){if(m.to==6){b[7]=b[5];b[5]=0;}else if(m.to==2){b[0]=b[3];b[3]=0;}else if(m.to==62){b[63]=b[61];b[61]=0;}else if(m.to==58){b[56]=b[59];b[59]=0;}}
        rehash();
    }

    vector<Move> legal(bool capturesOnly=false){
        vector<Move> out; auto ps=pseudo(capturesOnly); out.reserve(ps.size()); bool side=whiteTurn;
        for(auto &m:ps){State st;make(m,st);if(!inCheck(side))out.push_back(m);undo(m,st);}return out;
    }
};

struct TTEntry { uint64_t key=0; int depth=-1; int score=0; uint8_t flag=0; Move best{}; };
static constexpr uint8_t EXACT=0,LOWER=1,UPPER=2;

class Engine {
public:
    Board root=Board::start();
    vector<TTEntry> tt;
    uint64_t nodes=0;
    chrono::steady_clock::time_point deadline;
    atomic<bool> stop{false};
    int maxDepth=30, skill=20, aggression=60, overhead=30;
    long long timeLimitMs=1000;
    Move pv[MAX_PLY]{};
    Move killers[MAX_PLY][2]{};
    int history[2][64][64]{};

    Engine(){setHashMB(32);}
    void setHashMB(int mb){size_t n=max<size_t>(1,(size_t)mb*1024*1024/sizeof(TTEntry));tt.assign(n,{});}
    bool timeUp(){return stop.load() || chrono::steady_clock::now()>=deadline;}

    int pieceVal(int p) const { switch(absPiece(p)){case 1:return 100;case 2:return 320;case 3:return 335;case 4:return 500;case 5:return 900;case 6:return 20000;}return 0; }

    static int pst(int p,int s){
        int r=white(p)?rankOf(s):7-rankOf(s),f=fileOf(s);int a=absPiece(p);int v=0;
        if(a==1){v=r*10 - abs(3-f)*2; if(r==3)v+=18;}
        else if(a==2){static int t[8][8]={{-50,-30,-20,-20,-20,-20,-30,-50},{-20,0,5,8,8,5,0,-20},{-20,5,12,15,15,12,5,-20},{-10,8,15,20,20,15,8,-10},{-10,8,15,20,20,15,8,-10},{-20,5,12,15,15,12,5,-20},{-20,0,5,8,8,5,0,-20},{-50,-30,-20,-20,-20,-20,-30,-50}};v=t[r][f];}
        else if(a==3){v=6*min(r,7-r)+4*min(f,7-f);}
        else if(a==4){v=r*2 - abs(3-f);}
        else if(a==5){v=-(abs(3-f)+abs(3-r));}
        else if(a==6){v=(r<2?-(abs(3-f)*2):0); if(r>=5)v+=10-abs(3-f)*2;}
        return v;
    }

    int evaluate(const Board& b) const {
        int score=0,wmat=0,bmat=0,wmob=0,bmob=0,wpawns=0,bpawns=0, wk=-1,bk=-1;
        int filesW[8]={},filesB[8]={};
        for(int s=0;s<64;s++){int p=b.b[s];if(!p)continue;int v=pieceVal(p)+pst(p,s);if(p>0){score+=v;wmat+=pieceVal(p);if(p==WP){wpawns++;filesW[fileOf(s)]++;}if(p==WK)wk=s;}else{score-=v;bmat+=pieceVal(p);if(p==BP){bpawns++;filesB[fileOf(s)]++;}if(p==BK)bk=s;}}
        // Cheap mobility: pseudo attacks, deliberately avoiding legal move generation.
        for(int s=0;s<64;s++){int p=b.b[s];if(!p)continue;int f=fileOf(s),r=rankOf(s),a=absPiece(p);int m=0;
            if(a==1){int nr=r+(p>0?1:-1);for(int df:{-1,1}){int nf=f+df;if(nf>=0&&nf<8&&nr>=0&&nr<8)m++;}}
            else if(a==2){static const int df[8]={1,2,2,1,-1,-2,-2,-1},dr[8]={2,1,-1,-2,-2,-1,1,2};for(int i=0;i<8;i++){int nf=f+df[i],nr=r+dr[i];if(nf>=0&&nf<8&&nr>=0&&nr<8&&(!b.b[sq(nf,nr)]||white(b.b[sq(nf,nr)])!=white(p)))m++;}}
            else if(a==3||a==4||a==5){static const int df[8]={1,1,-1,-1,1,-1,0,0},dr[8]={1,-1,1,-1,0,0,1,-1};int begin=a==3?0:a==4?4:0,end=a==3?4:a==4?8:8;for(int d=begin;d<end;d++){int nf=f+df[d],nr=r+dr[d];while(nf>=0&&nf<8&&nr>=0&&nr<8){m++;if(b.b[sq(nf,nr)])break;nf+=df[d];nr+=dr[d];}}}
            else {for(int df=-1;df<=1;df++)for(int dr=-1;dr<=1;dr++)if(df||dr){int nf=f+df,nr=r+dr;if(nf>=0&&nf<8&&nr>=0&&nr<8)m++;}}
            if(p>0)wmob+=m;else bmob+=m;
        }
        score += (wmob-bmob)*3;
        for(int f=0;f<8;f++){if(filesW[f]==1)score-=12;if(filesB[f]==1)score+=12;if(filesW[f]==0&&filesB[f]>0)score-=3;if(filesB[f]==0&&filesW[f]>0)score+=3;}
        // Passed pawn bonus.
        for(int s=0;s<64;s++){int p=b.b[s];if(absPiece(p)!=1)continue;int f=fileOf(s),r=rankOf(s);bool passed=true;for(int nf=max(0,f-1);nf<=min(7,f+1);nf++){for(int rr=p>0?r+1:0; p>0?rr<8:rr<r; rr++){if(b.b[sq(nf,rr)]==(p>0?BP:WP)){passed=false;break;}}if(!passed)break;}if(passed)score += (p>0?1:-1)*(18+max(0,(p>0?r:7-r)-3)*12);}
        if(wk>=0&&bk>=0){int kdist=abs(fileOf(wk)-fileOf(bk))+abs(rankOf(wk)-rankOf(bk));if(wmat+bmat<2500)score += (kdist<5?(white(b.whiteTurn)?0:0):0);}
        if(b.whiteTurn) score += aggression*(wmob-bmob)/50; else score -= aggression*(wmob-bmob)/50;
        return b.whiteTurn?score:-score;
    }

    int captureScore(const Board& b,const Move& m) const { if(!m.capture && !(m.flags&EP))return 0;int victim=m.capture?pieceVal(m.capture):100;int attacker=pieceVal(m.piece);return 100000+victim*16-attacker; }

    void order(Board& b, vector<Move>& ms, const Move* ttMove=nullptr,int ply=0){
        for(auto &m:ms){m.score=0;if(ttMove&&m==*ttMove)m.score+=10000000;m.score+=captureScore(b,m);if(m.promo)m.score+=8000;if(ply<MAX_PLY){if(m==killers[ply][0])m.score+=500000;if(m==killers[ply][1])m.score+=450000;}m.score+=history[b.whiteTurn?1:0][m.from][m.to];}
        stable_sort(ms.begin(),ms.end(),[](const Move&a,const Move&b){return a.score>b.score;});
    }

    int quiesce(Board& b,int alpha,int beta,int ply){
        if(timeUp())return 0; nodes++;int stand=evaluate(b);if(stand>=beta)return beta;if(stand>alpha)alpha=stand;
        auto ms=b.legal(true);order(b,ms,nullptr,ply);for(auto&m:ms){State st;b.make(m,st);int score=-quiesce(b,-beta,-alpha,ply+1);b.undo(m,st);if(score>=beta)return beta;if(score>alpha)alpha=score;if(timeUp())break;}return alpha;
    }

    int search(Board& b,int depth,int alpha,int beta,int ply,bool allowNull=true){
        if(timeUp())return 0;nodes++; if(ply>=MAX_PLY-1)return evaluate(b);
        bool inChk=b.inCheck(b.whiteTurn);if(inChk)depth++;
        if(depth<=0)return quiesce(b,alpha,beta,ply);
        TTEntry &e=tt[b.key%tt.size()];Move ttMove{};bool hasTT=e.key==b.key&&e.depth>=0;if(hasTT)ttMove=e.best;
        if(hasTT&&e.depth>=depth){if(e.flag==EXACT)return e.score;if(e.flag==LOWER)alpha=max(alpha,e.score);else if(e.flag==UPPER)beta=min(beta,e.score);if(alpha>=beta)return e.score;}
        if(allowNull&&!inChk&&depth>=3){ // null move pruning
            State ns; Move nm; Board tmp=b; tmp.whiteTurn=!tmp.whiteTurn;tmp.ep=-1;tmp.halfmove++;tmp.rehash();int r=2+(depth>=6);int sc=-search(tmp,depth-r-1,-beta,-beta+1,ply+1,false);if(sc>=beta)return beta;
        }
        auto ms=b.legal(false);if(ms.empty())return inChk?(-MATE+ply):0;order(b,ms,hasTT?&ttMove:nullptr,ply);
        int origAlpha=alpha,best=-INF;Move bestMove{};int legalCount=0;
        for(size_t i=0;i<ms.size();i++){
            Move m=ms[i];State st;b.make(m,st);int sc;
            if(legalCount==0) sc=-search(b,depth-1,-beta,-alpha,ply+1,true);
            else {int reduction=(depth>=5&&legalCount>=4&&!inChk&&!m.capture&&!m.promo)?1:0;sc=-search(b,depth-1-reduction,-alpha-1,-alpha,ply+1,true);if(reduction&&sc>alpha)sc=-search(b,depth-1,-beta,-alpha,ply+1,true);if(sc>alpha&&sc<beta&&legalCount>0)sc=-search(b,depth-1,-beta,-alpha,ply+1,true);}
            b.undo(m,st);legalCount++;if(sc>best){best=sc;bestMove=m;}if(sc>alpha)alpha=sc;if(alpha>=beta){if(!m.capture&&!m.promo){if(!(m==killers[ply][0])){killers[ply][1]=killers[ply][0];killers[ply][0]=m;}history[b.whiteTurn?1:0][m.from][m.to]+=depth*depth*4;}break;}if(timeUp())break;
        }
        uint8_t flag=best<=origAlpha?UPPER:best>=beta?LOWER:EXACT;e={b.key,depth,best,flag,bestMove};return best;
    }

    Move think(int movetime=0){
        stop=false;nodes=0;deadline=chrono::steady_clock::now()+chrono::milliseconds(movetime>0?max(20,movetime):timeLimitMs);
        Move best{};int bestScore=-INF;vector<Move> rootMoves=root.legal(false);if(rootMoves.empty())return best;order(root,rootMoves,nullptr,0);
        int lastScore=0;
        for(int d=1;d<=maxDepth;d++){
            if(timeUp())break;int alpha=-INF,beta=INF; if(d>=4){alpha=lastScore-80;beta=lastScore+80;}
            Move iterBest=best;int iterScore=-INF;for(auto&m:rootMoves){if(timeUp())break;State st;root.make(m,st);int sc=-search(root,d-1,-beta,-alpha,1,true);root.undo(m,st);if(sc>iterScore){iterScore=sc;iterBest=m;}if(sc>alpha)alpha=sc;}
            if(timeUp())break;best=iterBest;bestScore=iterScore;lastScore=iterScore;order(root,rootMoves,&best,0);
            cout<<"info depth "<<d<<" score cp "<<bestScore<<" nodes "<<nodes<<" pv "<<uci(best)<<"\n"<<flush;
            if(abs(bestScore)>MATE-100)break;
        }
        return best;
    }

    string uci(const Move&m) const {string s; s+=char('a'+fileOf(m.from));s+=char('1'+rankOf(m.from));s+=char('a'+fileOf(m.to));s+=char('1'+rankOf(m.to));if(m.promo){int p=absPiece(m.promo);s+=p==5?'q':p==4?'r':p==3?'b':'n';}return s;}

    bool parseMove(const string&s,Move&out){if(s.size()<4)return false;int f1=s[0]-'a',r1=s[1]-'1',f2=s[2]-'a',r2=s[3]-'1';if(f1<0||f1>7||r1<0||r1>7||f2<0||f2>7||r2<0||r2>7)return false;int promo=0;if(s.size()>=5){switch(tolower((unsigned char)s[4])){case 'q':promo=WQ;break;case'r':promo=WR;break;case'b':promo=WB;break;case'n':promo=WN;break;default:return false;}}for(auto&m:root.legal(false))if(m.from==sq(f1,r1)&&m.to==sq(f2,r2)&&(promo==0||absPiece(m.promo)==promo)){out=m;return true;}return false;}

    void position(string line){stringstream ss(line);string tok;ss>>tok;Board b; if(tok=="startpos")b=Board::start();else if(tok=="fen"){string fen,part;for(int i=0;i<6&&ss>>part;i++){fen += (i?" ":"")+part;}if(!b.fromFEN(fen))return;}else return;string movesTok;ss>>movesTok;if(movesTok=="moves"){string ms;while(ss>>ms){Move m;if(parseMoveOn(b,ms,m)){State st;b.make(m,st);}else break;}}root=b;}
    bool parseMoveOn(Board&b,const string&s,Move&out){if(s.size()<4)return false;int f1=s[0]-'a',r1=s[1]-'1',f2=s[2]-'a',r2=s[3]-'1';int promo=0;if(s.size()>4){char c=tolower((unsigned char)s[4]);promo=c=='q'?5:c=='r'?4:c=='b'?3:c=='n'?2:0;}for(auto&m:b.legal(false))if(m.from==sq(f1,r1)&&m.to==sq(f2,r2)&&(promo==0||absPiece(m.promo)==promo)){out=m;return true;}return false;}
};

static int toInt(const string&s,int def){char*e=nullptr;long v=strtol(s.c_str(),&e,10);return (e&&*e==0)?(int)v:def;}

int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Engine e;
    string line;
    thread searchThread;
    bool searching=false;

    auto joinSearch=[&](){
        if(searchThread.joinable())
            searchThread.join();
        searching=false;
    };

    auto stopSearch=[&](){
        if(searching){
            e.stop.store(true);
            joinSearch();
        }
    };

    cout<<"SimpleLogics by DannyDwie\n"<<flush;

    while(getline(cin,line)){

        if(line=="uci"){
            cout
                <<"id name SimpleLogics\n"
                <<"id author DannyDwie\n"
                <<"option name Hash type spin default 32 min 1 max 512\n"
                <<"option name Threads type spin default 1 min 1 max 1\n"
                <<"option name Skill Level type spin default 20 min 0 max 20\n"
                <<"option name Aggression type spin default 60 min 0 max 100\n"
                <<"option name Search Depth type spin default 30 min 1 max 64\n"
                <<"option name Move Overhead type spin default 30 min 0 max 500\n"
                <<"uciok\n"<<flush;
        }

        else if(line=="isready"){
            stopSearch();
            cout<<"readyok\n"<<flush;
        }

        else if(line=="ucinewgame"){
            stopSearch();
            e.tt.assign(e.tt.size(),{});
            e.root=Board::start();
        }

        else if(line.rfind("setoption name ",0)==0){
            stopSearch();

            string rest=line.substr(15);
            string name,value;

            size_t p=rest.find(" value ");

            if(p!=string::npos){
                name=rest.substr(0,p);
                value=rest.substr(p+7);
            }else{
                name=rest;
            }

            if(name=="Hash"){
                e.setHashMB(max(1,toInt(value,32)));
            }
            else if(name=="Skill Level"){
                e.skill=max(0,min(20,toInt(value,20)));
            }
            else if(name=="Aggression"){
                e.aggression=max(0,min(100,toInt(value,60)));
            }
            else if(name=="Search Depth"){
                e.maxDepth=max(1,min(64,toInt(value,30)));
            }
            else if(name=="Move Overhead"){
                e.overhead=max(0,min(500,toInt(value,30)));
            }
        }

        else if(line.rfind("position ",0)==0){
            stopSearch();
            e.position(line.substr(9));
        }

        else if(line.rfind("go",0)==0){
            stopSearch();

            stringstream ss(line);
            string t;
            ss>>t;

            int movetime=0;
            int depth=0;
            int wtime=-1;
            int btime=-1;
            int winc=0;
            int binc=0;
            bool infinite=false;

            while(ss>>t){
                if(t=="movetime"){
                    ss>>movetime;
                }
                else if(t=="depth"){
                    ss>>depth;
                }
                else if(t=="wtime"){
                    ss>>wtime;
                }
                else if(t=="btime"){
                    ss>>btime;
                }
                else if(t=="winc"){
                    ss>>winc;
                }
                else if(t=="binc"){
                    ss>>binc;
                }
                else if(t=="infinite"){
                    infinite=true;
                }
            }

            if(depth>0)
                e.maxDepth=min(64,depth);

            int mt=1000;

            if(movetime>0){
                mt=movetime;
            }
            else if(infinite){
                mt=600000;
            }
            else{
                int remain=
                    e.root.whiteTurn?wtime:btime;

                int inc=
                    e.root.whiteTurn?winc:binc;

                if(remain>=0){
                    mt=remain/25+(inc*3)/4;
                    mt=max(100,mt);

                    if(remain>e.overhead+100){
                        mt=min(
                            mt,
                            remain-e.overhead-50
                        );
                    }
                }
            }

            int actual=max(50,mt-e.overhead);

            e.stop.store(false);
            searching=true;

            searchThread=thread(
                [&e,actual,&searching](){
                    Move m=e.think(actual);

                    if(!e.stop.load()){
                        if(m.from!=m.to){
                            cout
                                <<"bestmove "
                                <<e.uci(m)
                                <<"\n"<<flush;
                        }else{
                            cout
                                <<"bestmove 0000\n"
                                <<flush;
                        }
                    }

                }
            );
        }

        else if(line=="stop"){
            stopSearch();
        }

        else if(line=="quit"){
            stopSearch();
            break;
        }
    }

    stopSearch();
    return 0;
}
