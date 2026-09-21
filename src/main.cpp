#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <random>

using namespace std;

enum Piece : int { EMPTY=0, WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6, BP=-1, BN=-2, BB=-3, BR=-4, BQ=-5, BK=-6 };
enum Flag : uint8_t { QUIET=0, CAPTURE=1, DOUBLE_PUSH=2, EP_CAPTURE=4, KING_CASTLE=8, QUEEN_CASTLE=16, PROMOTION=32 };

struct Move { uint8_t from=0,to=0; int8_t promotion=0; uint8_t flags=QUIET; };
static bool sameMove(const Move&a,const Move&b){ return a.from==b.from&&a.to==b.to&&a.promotion==b.promotion; }

struct Board {
    array<int,64> b{}; bool white=true; uint8_t castle=15; int ep=-1; int halfmove=0; int fullmove=1;
    static Board startpos(){ Board x; x.b.fill(EMPTY); const int back[8]={WR,WN,WB,WQ,WK,WB,WN,WR}; for(int i=0;i<8;i++){x.b[i]=back[i];x.b[8+i]=WP;x.b[48+i]=BP;x.b[56+i]=-back[i];} return x; }
};

struct Settings { int hashMB=32, skill=20, aggression=50, searchDepth=0, moveOverhead=50, style=0; };
static Settings cfg;
static atomic<bool> stopSearch(false);
static chrono::steady_clock::time_point deadline=chrono::steady_clock::time_point::max();
static uint64_t nodes=0;

static inline int fileOf(int s){return s&7;} static inline int rankOf(int s){return s>>3;}
static inline bool inside(int f,int r){return f>=0&&f<8&&r>=0&&r<8;}
static inline bool own(int p,bool w){return w?p>0:p<0;} static inline bool enemy(int p,bool w){return w?p<0:p>0;}
static inline int ptype(int p){return abs(p);} 
static int pieceValue(int p){switch(abs(p)){case 1:return 100;case 2:return 320;case 3:return 335;case 4:return 500;case 5:return 900;case 6:return 20000;}return 0;}
static string sq(int s){string r="a1";r[0]=char('a'+fileOf(s));r[1]=char('1'+rankOf(s));return r;}
static string uciMove(const Move&m){string r=sq(m.from)+sq(m.to);if(m.promotion){switch(abs((int)m.promotion)){case 5:r+='q';break;case 4:r+='r';break;case 3:r+='b';break;case 2:r+='n';break;}}return r;}

static uint64_t zobrist[12][64], zobSide, zobCastle[16], zobEp[64];
static uint64_t rng64(){static uint64_t x=0x9e3779b97f4a7c15ULL;x^=x>>12;x^=x<<25;x^=x>>27;return x*0x2545F4914F6CDD1DULL;}
static void initZobrist(){for(auto&r:zobrist)for(auto&v:r)v=rng64();zobSide=rng64();for(auto&v:zobCastle)v=rng64();for(auto&v:zobEp)v=rng64();}
static uint64_t keyOf(const Board&x){uint64_t k=0;for(int s=0;s<64;s++){int p=x.b[s];if(!p)continue;int idx=p>0?p-1:6+(-p-1);k^=zobrist[idx][s];}if(x.white)k^=zobSide;k^=zobCastle[x.castle];if(x.ep>=0)k^=zobEp[x.ep];return k;}

static int kingSquare(const Board&x,bool w){int k=w?WK:BK;for(int s=0;s<64;s++)if(x.b[s]==k)return s;return -1;}
static bool attacked(const Board&x,int s,bool byWhite){
    int f=fileOf(s),r=rankOf(s),pr=byWhite?r-1:r+1;
    if(pr>=0&&pr<8)for(int df:{-1,1}){int nf=f+df;if(inside(nf,pr)&&x.b[pr*8+nf]==(byWhite?WP:BP))return true;}
    static const int kn[8][2]={{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    for(auto d:kn){int nf=f+d[0],nr=r+d[1];if(inside(nf,nr)&&x.b[nr*8+nf]==(byWhite?WN:BN))return true;}
    static const int diag[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
    for(auto d:diag){int nf=f,nr=r;while(true){nf+=d[0];nr+=d[1];if(!inside(nf,nr))break;int p=x.b[nr*8+nf];if(p){if(p==(byWhite?WB:BB)||p==(byWhite?WQ:BQ))return true;break;}}}
    static const int ortho[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(auto d:ortho){int nf=f,nr=r;while(true){nf+=d[0];nr+=d[1];if(!inside(nf,nr))break;int p=x.b[nr*8+nf];if(p){if(p==(byWhite?WR:BR)||p==(byWhite?WQ:BQ))return true;break;}}}
    for(int df=-1;df<=1;df++)for(int dr=-1;dr<=1;dr++)if((df||dr)&&inside(f+df,r+dr)&&x.b[(r+dr)*8+f+df]==(byWhite?WK:BK))return true;
    return false;
}
static bool inCheck(const Board&x,bool w){int k=kingSquare(x,w);return k<0||attacked(x,k,!w);}
static void addMove(vector<Move>&m,int f,int t,uint8_t flags=QUIET,int promo=0){m.push_back(Move{(uint8_t)f,(uint8_t)t,(int8_t)promo,flags});}

static void pseudo(const Board&x,vector<Move>&m){
    m.clear();bool w=x.white;
    for(int s=0;s<64;s++){
        int p=x.b[s];if(!own(p,w))continue;int f=fileOf(s),r=rankOf(s),a=abs(p);
        if(a==1){
            int d=w?1:-1,nr=r+d;if(!inside(f,nr))continue;int t=nr*8+f;
            if(x.b[t]==EMPTY){
                if(nr==0||nr==7)for(int q:{w?WQ:BQ,w?WR:BR,w?WB:BB,w?WN:BN})addMove(m,s,t,PROMOTION,q);
                else addMove(m,s,t);
                int sr=w?1:6,nr2=r+2*d;if(r==sr&&x.b[nr2*8+f]==EMPTY)addMove(m,s,nr2*8+f,DOUBLE_PUSH);
            }
            for(int df:{-1,1}){int nf=f+df;if(!inside(nf,nr))continue;t=nr*8+nf;if(enemy(x.b[t],w)&&abs(x.b[t])!=WK){if(nr==0||nr==7)for(int q:{w?WQ:BQ,w?WR:BR,w?WB:BB,w?WN:BN})addMove(m,s,t,CAPTURE|PROMOTION,q);else addMove(m,s,t,CAPTURE);}if(t==x.ep)addMove(m,s,t,EP_CAPTURE);}
        } else if(a==2){
            static const int d[8][2]={{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};for(auto z:d){int nf=f+z[0],nr=r+z[1];if(!inside(nf,nr))continue;int t=nr*8+nf;if(!own(x.b[t],w)&&abs(x.b[t])!=WK)addMove(m,s,t,x.b[t]?CAPTURE:QUIET);}
        } else if(a==3||a==4||a==5){
            static const int dirs[8][2]={{1,1},{1,-1},{-1,1},{-1,-1},{1,0},{-1,0},{0,1},{0,-1}};int first=a==4?4:0,last=a==3?4:8;for(int di=first;di<last;di++){int nf=f,nr=r;while(true){nf+=dirs[di][0];nr+=dirs[di][1];if(!inside(nf,nr))break;int t=nr*8+nf;if(x.b[t]==EMPTY)addMove(m,s,t);else{if(enemy(x.b[t],w)&&abs(x.b[t])!=WK)addMove(m,s,t,CAPTURE);break;}}}
        } else if(a==6){
            for(int df=-1;df<=1;df++)for(int dr=-1;dr<=1;dr++){if(!df&&!dr)continue;int nf=f+df,nr=r+dr;if(!inside(nf,nr))continue;int t=nr*8+nf;if(!own(x.b[t],w)&&abs(x.b[t])!=WK)addMove(m,s,t,x.b[t]?CAPTURE:QUIET);}
            if(w&&s==4&&!inCheck(x,true)){
                if((x.castle&1)&&x.b[5]==EMPTY&&x.b[6]==EMPTY&&x.b[7]==WR&&!attacked(x,5,false)&&!attacked(x,6,false))addMove(m,4,6,KING_CASTLE);
                if((x.castle&2)&&x.b[1]==EMPTY&&x.b[2]==EMPTY&&x.b[3]==EMPTY&&x.b[0]==WR&&!attacked(x,3,false)&&!attacked(x,2,false))addMove(m,4,2,QUEEN_CASTLE);
            }
            if(!w&&s==60&&!inCheck(x,false)){
                if((x.castle&4)&&x.b[61]==EMPTY&&x.b[62]==EMPTY&&x.b[63]==BR&&!attacked(x,61,true)&&!attacked(x,62,true))addMove(m,60,62,KING_CASTLE);
                if((x.castle&8)&&x.b[57]==EMPTY&&x.b[58]==EMPTY&&x.b[59]==EMPTY&&x.b[56]==BR&&!attacked(x,59,true)&&!attacked(x,58,true))addMove(m,60,58,QUEEN_CASTLE);
            }
        }
    }
}

static Board makeMove(const Board&x,const Move&m){
    Board n=x;int p=n.b[m.from],cap=n.b[m.to];n.ep=-1;n.halfmove++;if(abs(p)==1||cap||(m.flags&EP_CAPTURE))n.halfmove=0;n.b[m.to]=p;n.b[m.from]=EMPTY;
    if(m.flags&EP_CAPTURE)n.b[m.to+(x.white?-8:8)]=EMPTY;
    if(m.flags&KING_CASTLE){if(x.white){n.b[5]=WR;n.b[7]=EMPTY;}else{n.b[61]=BR;n.b[63]=EMPTY;}}
    if(m.flags&QUEEN_CASTLE){if(x.white){n.b[3]=WR;n.b[0]=EMPTY;}else{n.b[59]=BR;n.b[56]=EMPTY;}}
    if(m.promotion)n.b[m.to]=m.promotion;if(m.flags&DOUBLE_PUSH)n.ep=m.from+(x.white?8:-8);
    if(p==WK)n.castle&=~3;if(p==BK)n.castle&=~12;if(m.from==0||m.to==0)n.castle&=~2;if(m.from==7||m.to==7)n.castle&=~1;if(m.from==56||m.to==56)n.castle&=~8;if(m.from==63||m.to==63)n.castle&=~4;
    n.white=!x.white;if(n.white)n.fullmove++;return n;
}
static vector<Move> legalMoves(const Board&x){vector<Move> p,l;pseudo(x,p);for(const Move&m:p){Board n=makeMove(x,m);if(!inCheck(n,x.white))l.push_back(m);}return l;}

static bool insufficientMaterial(const Board&x){int nonKing=0,bishops=0,knights=0,color=-1;for(int s=0;s<64;s++){int p=x.b[s],a=abs(p);if(!p||a==6)continue;if(a==1||a==4||a==5)return false;nonKing++;if(a==3){bishops++;int c=(fileOf(s)+rankOf(s))&1;if(color<0)color=c;else if(color!=c)return false;}else if(a==2)knights++;else return false;}if(nonKing<=1)return true;if(nonKing==2&&(bishops==2||knights==2))return true;return false;}

static int pstPawn[64]={0,0,0,0,0,0,0,0,5,10,10,-20,-20,10,10,5,5,-5,-10,0,0,-10,-5,5,0,0,0,20,20,0,0,0,5,5,10,25,25,10,5,5,10,10,20,30,30,20,10,10,50,50,50,50,50,50,50,50,0,0,0,0,0,0,0,0};
static int pstKnight[64]={-50,-40,-30,-30,-30,-30,-40,-50,-40,-20,0,5,5,0,-20,-40,-30,5,10,15,15,10,5,-30,-30,0,15,20,20,15,0,-30,-30,5,15,20,20,15,5,-30,-30,0,10,15,15,10,0,-30,-40,-20,0,0,0,0,-20,-40,-50,-40,-30,-30,-30,-30,-40,-50};
static int pstBishop[64]={-20,-10,-10,-10,-10,-10,-10,-20,-10,5,0,0,0,0,5,-10,-10,10,10,10,10,10,10,-10,-10,0,10,10,10,10,0,-10,-10,5,5,10,10,5,5,-10,-10,0,5,10,10,5,0,-10,-10,0,0,0,0,0,0,-10,-20,-10,-10,-10,-10,-10,-10,-20};
static int pstRook[64]={0,0,0,5,5,0,0,0,-5,0,0,0,0,0,0,-5,-5,0,0,0,0,0,0,-5,-5,0,0,0,0,0,0,-5,-5,0,0,0,0,0,0,-5,-5,0,0,0,0,0,0,-5,5,10,10,10,10,10,10,5,0,0,0,0,0,0,0,0};
static int pstQueen[64]={-20,-10,-10,0,0,-10,-10,-20,-10,0,0,0,0,0,0,-10,-10,0,5,5,5,5,0,-10,0,0,5,5,5,5,0,-5,0,0,5,5,5,5,0,-5,-10,5,5,5,5,5,0,-10,-10,0,5,0,0,0,0,-10,-20,-10,-10,0,0,-10,-10,-20};
static int pstKing[64]={-30,-40,-40,-50,-50,-40,-40,-30,-30,-40,-40,-50,-50,-40,-40,-30,-30,-40,-40,-50,-50,-40,-40,-30,-30,-40,-40,-50,-50,-40,-40,-30,-20,-30,-30,-40,-40,-30,-30,-20,-10,-20,-20,-20,-20,-20,-20,-10,20,20,0,0,0,0,20,20,20,30,10,0,0,10,30,20};
static int pstEndKing[64]={-50,-30,-30,-30,-30,-30,-30,-50,-30,-10,0,0,0,0,-10,-30,-30,0,10,20,20,10,0,-30,-30,0,20,30,30,20,0,-30,-30,0,20,30,30,20,0,-30,-30,0,10,20,20,10,0,-30,-30,-10,0,0,0,0,-10,-30,-50,-30,-30,-30,-30,-30,-30,-50};
static int mirrorSq(int s){return (7-rankOf(s))*8+fileOf(s);}

static int evaluate(const Board&x){
    int score=0,nonKingMaterial=0,whiteB=0,blackB=0;
    for(int s=0;s<64;s++){
        int p=x.b[s];if(!p)continue;int a=abs(p);int v=pieceValue(p);if(a!=6)nonKingMaterial+=v;
        int ps=0,idx=p>0?s:mirrorSq(s);if(a==1)ps=pstPawn[idx];else if(a==2)ps=pstKnight[idx];else if(a==3)ps=pstBishop[idx];else if(a==4)ps=pstRook[idx];else if(a==5)ps=pstQueen[idx];else if(a==6)ps=(nonKingMaterial<=4500?pstEndKing[idx]:pstKing[idx]);
        score += p>0 ? v+ps : -(v+ps);if(p==WB)whiteB++;if(p==BB)blackB++;
    }
    if(whiteB>=2)score+=30;if(blackB>=2)score-=30;
    int whiteMob=0,blackMob=0;{Board y=x;y.white=true;whiteMob=(int)legalMoves(y).size();y.white=false;blackMob=(int)legalMoves(y).size();}
    score += (whiteMob-blackMob)*3;
    for(int s=0;s<64;s++){int p=x.b[s];if(abs(p)!=1)continue;int f=fileOf(s),r=rankOf(s);bool w=p>0;int rr=w?r:7-r;int advance=max(0,rr-1);score += w?advance*4:-advance*4;if(rr>=5)score+=w?8:-8;}
    int ag=cfg.aggression;if(cfg.style==1)ag=max(ag,75);if(cfg.style==2)ag=max(ag,90);if(cfg.style==3)ag=min(ag,45);
    score += (ag-50)*(whiteMob-blackMob)/8;
    if(nonKingMaterial>7000){int wk=kingSquare(x,true),bk=kingSquare(x,false);if(wk>=0){int f=fileOf(wk),r=rankOf(wk);if(f>=2&&f<=5&&r<=2)score-=max(0,ag-40)/2;}if(bk>=0){int f=fileOf(bk),r=rankOf(bk);if(f>=2&&f<=5&&r>=5)score+=max(0,ag-40)/2;}}
    else if(nonKingMaterial<=4500){int wk=kingSquare(x,true),bk=kingSquare(x,false);if(wk>=0){int d=abs(fileOf(wk)-3)+abs(rankOf(wk)-3);score+=(14-d*3);}if(bk>=0){int d=abs(fileOf(bk)-3)+abs(rankOf(bk)-3);score-=(14-d*3);}}
    if(cfg.style==2)score += (score>=0?8:-8); else if(cfg.style==1)score += (score>=0?4:-4);
    return x.white?score:-score;
}

struct TTEntry {uint64_t key=0;int depth=-1;int score=0;uint8_t flag=0;Move best{};};
static vector<TTEntry> tt;
static void resizeTT(int mb){size_t bytes=(size_t)max(1,mb)*1024ULL*1024ULL;size_t n=max<size_t>(1,bytes/sizeof(TTEntry));tt.assign(n,TTEntry{});}
static bool probeTT(uint64_t k,int depth,int alpha,int beta,int&score,Move&best){if(tt.empty())return false;auto&e=tt[k%tt.size()];if(e.key!=k||e.depth<depth)return false;best=e.best;if(e.flag==0){score=e.score;return true;}if(e.flag==1&&e.score<=alpha){score=e.score;return true;}if(e.flag==2&&e.score>=beta){score=e.score;return true;}return false;}
static void storeTT(uint64_t k,int depth,int score,uint8_t flag,const Move&best){if(tt.empty())return;auto&e=tt[k%tt.size()];if(depth>=e.depth||e.key!=k){e={k,depth,score,flag,best};}}

static Move killer[64][2];
static int historyTable[2][64][64]{};
static int moveOrder(const Board&x,const Move&m,const Move&pv){int s=0;if(sameMove(m,pv))s+=1000000;if(m.flags&(CAPTURE|EP_CAPTURE))s+=500000+pieceValue(x.b[m.to])*10-pieceValue(x.b[m.from]);if(m.promotion)s+=800000+pieceValue(m.promotion);if(sameMove(m,killer[0][0]))s+=5000;if(sameMove(m,killer[0][1]))s+=4000;s+=historyTable[x.white?0:1][m.from][m.to]/16;return s;}
static void orderMoves(const Board&x,vector<Move>&m,const Move&pv){stable_sort(m.begin(),m.end(),[&](const Move&a,const Move&b){return moveOrder(x,a,pv)>moveOrder(x,b,pv);});}
static bool shouldStop(){if(stopSearch.load())return true;if((++nodes&2047ULL)==0&&chrono::steady_clock::now()>=deadline){stopSearch.store(true);return true;}return false;}

static int quiescence(const Board&x,int alpha,int beta,int ply){if(shouldStop())return 0;int stand=evaluate(x);if(stand>=beta)return beta;if(stand>alpha)alpha=stand;auto moves=legalMoves(x);orderMoves(x,moves,Move{});for(const Move&m:moves){if(!(m.flags&(CAPTURE|EP_CAPTURE|PROMOTION)))continue;Board n=makeMove(x,m);int score=-quiescence(n,-beta,-alpha,ply+1);if(score>=beta)return beta;if(score>alpha)alpha=score;if(ply>=10)break;}return alpha;}

static int search(const Board&x,int depth,int alpha,int beta,int ply,bool allowNull,Move*pvBest){
    if(shouldStop())return 0;bool check=inCheck(x,x.white);if(depth<=0)return quiescence(x,alpha,beta,ply);if(x.halfmove>=100||insufficientMaterial(x))return 0;
    uint64_t k=keyOf(x);Move ttBest{};int ttScore=0;if(probeTT(k,depth,alpha,beta,ttScore,ttBest))return ttScore;
    if(allowNull&&depth>=3&&!check){Board n=x;n.white=!x.white;n.ep=-1;int r=2;int score=-search(n,depth-1-r,-beta,-beta+1,ply+1,false,nullptr);if(score>=beta)return beta;}
    auto moves=legalMoves(x);if(moves.empty())return check?(-10000000+ply):0;orderMoves(x,moves,ttBest);int originalAlpha=alpha,bestScore=-10000000;Move bestMove=moves[0];int moveNo=0;
    for(const Move&m:moves){if(shouldStop())break;Board n=makeMove(x,m);int nextDepth=depth-1;if(check&&depth<12)nextDepth++;int score;
        bool tactical=m.flags&(CAPTURE|EP_CAPTURE|PROMOTION);if(moveNo==0)score=-search(n,nextDepth,-beta,-alpha,ply+1,true,nullptr);else{int reduced=nextDepth;if(moveNo>=4&&!tactical&&depth>=3)reduced=max(0,reduced-1);score=-search(n,reduced,-alpha-1,-alpha,ply+1,true,nullptr);if(score>alpha&&score<beta)score=-search(n,nextDepth,-beta,-alpha,ply+1,true,nullptr);}moveNo++;
        if(score>bestScore){bestScore=score;bestMove=m;}if(score>alpha){alpha=score;if(pvBest)*pvBest=m;}if(alpha>=beta){if(!tactical){killer[min(ply,63)][1]=killer[min(ply,63)][0];killer[min(ply,63)][0]=m;int side=x.white?0:1;historyTable[side][m.from][m.to]=min(100000,historyTable[side][m.from][m.to]+depth*depth*4);}break;}
    }
    if(shouldStop())return bestScore;uint8_t flag=bestScore<=originalAlpha?1:(bestScore>=beta?2:0);storeTT(k,depth,bestScore,flag,bestMove);return bestScore;
}

static Move think(const Board&x,int maxDepth){auto root=legalMoves(x);if(root.empty())return Move{};Move best=root[0];int completed=0;nodes=0;for(int d=1;d<=maxDepth;d++){if(shouldStop())break;Move cur=best;int score=search(x,d,-10000000,10000000,0,true,&cur);if(shouldStop())break;best=cur;completed=d;cout<<"info depth "<<d<<" score cp "<<score<<" nodes "<<nodes<<" pv "<<uciMove(best)<<'\n'<<flush;if(cfg.skill<=3&&d>=4)break;if(cfg.skill<=7&&d>=6)break;}if(!completed)best=root[0];return best;}

static int parseInt(const string&s,int fallback){try{return stoi(s);}catch(...){return fallback;}}
static string lowerString(string s){for(char&c:s)c=(char)tolower((unsigned char)c);return s;}
static int parseStyle(const string&s){string v=lowerString(s);if(v=="aggressive")return 1;if(v=="tal")return 2;if(v=="human")return 3;return 0;}
static void applyStyle(){if(cfg.style==1)cfg.aggression=max(cfg.aggression,75);else if(cfg.style==2)cfg.aggression=max(cfg.aggression,90);else if(cfg.style==3)cfg.aggression=min(cfg.aggression,45);}
static void setOptionLine(const string&line){if(line.rfind("setoption name ",0)!=0)return;string s=line.substr(15);size_t pos=s.find(" value ");string name=pos==string::npos?s:s.substr(0,pos);string value=pos==string::npos?"":s.substr(pos+7);while(!name.empty()&&isspace((unsigned char)name.back()))name.pop_back();while(!value.empty()&&isspace((unsigned char)value.front()))value.erase(value.begin());string n=lowerString(name);if(n=="hash"){cfg.hashMB=max(1,min(512,parseInt(value,32)));resizeTT(cfg.hashMB);}else if(n=="skill level")cfg.skill=max(0,min(20,parseInt(value,20)));else if(n=="aggression")cfg.aggression=max(0,min(100,parseInt(value,50)));else if(n=="search depth")cfg.searchDepth=max(0,min(64,parseInt(value,0)));else if(n=="move overhead")cfg.moveOverhead=max(0,min(1000,parseInt(value,50)));else if(n=="style"){cfg.style=parseStyle(value);applyStyle();}}

struct Go {long long wtime=-1,btime=-1,winc=0,binc=0,movetime=-1;int depth=0;bool infinite=false;};
static Go parseGo(const string&line){Go g;stringstream ss(line);string t;ss>>t;while(ss>>t){if(t=="wtime")ss>>g.wtime;else if(t=="btime")ss>>g.btime;else if(t=="winc")ss>>g.winc;else if(t=="binc")ss>>g.binc;else if(t=="movetime")ss>>g.movetime;else if(t=="depth")ss>>g.depth;else if(t=="infinite")g.infinite=true;}return g;}
static int timeBudget(const Board&x,const Go&g){if(g.movetime>=0)return max(20,(int)g.movetime-cfg.moveOverhead);if(g.infinite)return 0;long long t=x.white?g.wtime:g.btime,inc=x.white?g.winc:g.binc;if(t<0)return 500;long long b=t/32+inc*3/4;b=max(30LL,b);b=min(b,max(30LL,t/3));b=min(b,5000LL);b-=cfg.moveOverhead;return (int)max(20LL,b);}

static bool parseSquare(const string&s,int&out){if(s.size()!=2||s[0]<'a'||s[0]>'h'||s[1]<'1'||s[1]>'8')return false;out=(s[1]-'1')*8+(s[0]-'a');return true;}
static bool parseFEN(const string&fen,Board&b){vector<string>p;stringstream ss(fen);string t;while(ss>>t)p.push_back(t);if(p.size()<4)return false;Board n;n.b.fill(EMPTY);int r=7,f=0;for(char c:p[0]){if(c=='/'){if(f!=8)return false;--r;f=0;continue;}if(isdigit((unsigned char)c)){f+=c-'0';if(f>8)return false;continue;}int q=0;switch(c){case'P':q=WP;break;case'N':q=WN;break;case'B':q=WB;break;case'R':q=WR;break;case'Q':q=WQ;break;case'K':q=WK;break;case'p':q=BP;break;case'n':q=BN;break;case'b':q=BB;break;case'r':q=BR;break;case'q':q=BQ;break;case'k':q=BK;break;default:return false;}if(f>=8||r<0)return false;n.b[r*8+f++]=q;}if(r!=0||f!=8)return false;n.white=p[1]=="w";n.castle=0;if(p[2]!="-")for(char c:p[2]){if(c=='K')n.castle|=1;else if(c=='Q')n.castle|=2;else if(c=='k')n.castle|=4;else if(c=='q')n.castle|=8;}n.ep=-1;if(p[3]!="-"&&!parseSquare(p[3],n.ep))return false;if(p.size()>4)n.halfmove=max(0,parseInt(p[4],0));if(p.size()>5)n.fullmove=max(1,parseInt(p[5],1));b=n;return true;}
static bool parseMoveString(Board&b,const string&u){if(u.size()<4)return false;int from,to;if(!parseSquare(u.substr(0,2),from)||!parseSquare(u.substr(2,2),to))return false;int promo=EMPTY;if(u.size()>=5){char c=(char)tolower((unsigned char)u[4]);if(b.white){if(c=='q')promo=WQ;else if(c=='r')promo=WR;else if(c=='b')promo=WB;else if(c=='n')promo=WN;}else{if(c=='q')promo=BQ;else if(c=='r')promo=BR;else if(c=='b')promo=BB;else if(c=='n')promo=BN;}}for(const Move&m:legalMoves(b))if(m.from==from&&m.to==to&&m.promotion==promo){b=makeMove(b,m);return true;}return false;}
static void setPosition(Board&b,const string&line){stringstream ss(line);string t;ss>>t;if(!(ss>>t))return;if(t=="startpos"){b=Board::startpos();if(ss>>t&&t=="moves"){string m;while(ss>>m)parseMoveString(b,m);}return;}if(t=="fen"){vector<string>fp;while(ss>>t){if(t=="moves")break;fp.push_back(t);if(fp.size()==6)break;}if(fp.size()<4)return;string fen;for(size_t i=0;i<fp.size();i++){if(i)fen+=' ';fen+=fp[i];}Board n=b;if(parseFEN(fen,n)){b=n;if(t=="moves"){string m;while(ss>>m)parseMoveString(b,m);}else if(ss>>t&&t=="moves"){string m;while(ss>>m)parseMoveString(b,m);}}}}

static void printUCI(){cout<<"id name SimpleLogics Final"<<endl;cout<<"id author Danny"<<endl;cout<<"option name Hash type spin default 32 min 1 max 512"<<endl;cout<<"option name Skill Level type spin default 20 min 0 max 20"<<endl;cout<<"option name Aggression type spin default 50 min 0 max 100"<<endl;cout<<"option name Search Depth type spin default 0 min 0 max 64"<<endl;cout<<"option name Move Overhead type spin default 50 min 0 max 1000"<<endl;cout<<"option name Style type combo default Normal var Normal var Aggressive var Tal var Human"<<endl;cout<<"uciok"<<endl;}
static int effectiveDepth(const Go&g){int d=g.depth>0?g.depth:(cfg.searchDepth>0?cfg.searchDepth:64);if(cfg.skill<20){int sd=cfg.skill<=2?2:cfg.skill<=5?3:cfg.skill<=8?4:cfg.skill<=11?5:cfg.skill<=14?6:cfg.skill<=17?7:8;d=min(d,sd);}return max(1,d);}
static void handleGo(const Board&b,const string&line){Go g=parseGo(line);int ms=timeBudget(b,g);stopSearch.store(false);deadline=ms>0?chrono::steady_clock::now()+chrono::milliseconds(ms):chrono::steady_clock::time_point::max();int depth=effectiveDepth(g);Move best=think(b,depth);stopSearch.store(true);cout<<"bestmove "<<uciMove(best)<<endl;}
static void prepareEngine(){initZobrist();resizeTT(cfg.hashMB);for(auto&r:killer)r[0]=r[1]=Move{};for(auto&s:historyTable)for(auto&r:s)for(auto&v:r)v=0;stopSearch.store(false);}
static void runUCI(){prepareEngine();Board board=Board::startpos();string line;while(getline(cin,line)){if(line.empty())continue;stringstream ss(line);string cmd;ss>>cmd;if(cmd=="uci")printUCI();else if(cmd=="isready")cout<<"readyok"<<endl;else if(cmd=="setoption")setOptionLine(line);else if(cmd=="ucinewgame"){stopSearch.store(true);resizeTT(cfg.hashMB);for(auto&r:killer)r[0]=r[1]=Move{};for(auto&s:historyTable)for(auto&r:s)for(auto&v:r)v=0;}else if(cmd=="position")setPosition(board,line);else if(cmd=="go")handleGo(board,line);else if(cmd=="stop")stopSearch.store(true);else if(cmd=="quit"){stopSearch.store(true);break;}}}
int main(){ios::sync_with_stdio(false);cin.tie(nullptr);runUCI();return 0;}
