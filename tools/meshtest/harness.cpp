// Offline test of code/rd-vanilla/tr_meshcloth.cpp on the real zz_palpatine
// model with real _humanoid_pal animations, skinned exactly like
// RB_SurfaceGhoul / G2_TransformBone (lerped 3x4 matrices, parent chain,
// identity root). Prints measurements and writes preview data.
#include "../md/code/qcommon/q_shared.h"
#include "../md/code/rd-vanilla/tr_meshcloth.h"
#include <cstdio>
#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <algorithm>
#include <functional>
#include <chrono>

#include "../md/code/qcommon/matcomp.h"

static std::vector<unsigned char> readFile(const char* p){ FILE*f=fopen(p,"rb"); std::vector<unsigned char> b; if(!f){perror(p);exit(1);} fseek(f,0,SEEK_END); b.resize(ftell(f)); fseek(f,0,SEEK_SET); fread(b.data(),1,b.size(),f); fclose(f); return b; }

static void Mul34(mdxaBone_t* out, const mdxaBone_t* a, const mdxaBone_t* b){
	for(int i=0;i<3;i++){ for(int j=0;j<3;j++) out->matrix[i][j]=a->matrix[i][0]*b->matrix[0][j]+a->matrix[i][1]*b->matrix[1][j]+a->matrix[i][2]*b->matrix[2][j];
		out->matrix[i][3]=a->matrix[i][0]*b->matrix[0][3]+a->matrix[i][1]*b->matrix[1][3]+a->matrix[i][2]*b->matrix[2][3]+a->matrix[i][3]; }
}

struct Anim { const char* name; int first, num, fps; };
struct Gla { std::vector<unsigned char> d; const mdxaHeader_t* h; std::vector<const mdxaSkel_t*> skel; std::map<std::string,int> byName; };
static Gla G;

static void animLocal(int bone, int frame, mdxaBone_t* out){
	const unsigned char* base=G.d.data();
	if(frame>=G.h->numFrames) frame=G.h->numFrames-1;
	int off=G.h->ofsFrames + (frame*G.h->numBones + bone)*3;
	int idx= base[off] | (base[off+1]<<8) | (base[off+2]<<16);
	const unsigned char* comp = base + G.h->ofsCompBonePool + idx*14;
	MC_UnCompressQuat(out->matrix, comp);
}

// local animated matrix for a bone: anim A at time ta blended with anim B by w
static void localAt(int bone, const Anim& A, float ta, mdxaBone_t* out){
	float fr = ta*A.fps; int n=abs(A.num); float ff=fmodf(fr,(float)n); int f0=(int)ff, f1=(f0+1)%n; float bl=ff-f0;
	mdxaBone_t m0,m1; animLocal(bone,A.first+f0,&m0); animLocal(bone,A.first+f1,&m1);
	for(int k=0;k<12;k++) ((float*)out)[k]=((float*)&m0)[k]*(1-bl)+((float*)&m1)[k]*bl;
}

static void evalBones(const Anim& A, float ta, const Anim* B, float tb, float w, std::vector<mdxaBone_t>& fin){
	int nb=G.h->numBones; fin.resize(nb);
	mdxaBone_t root; memset(&root,0,sizeof root); root.matrix[0][0]=root.matrix[1][1]=root.matrix[2][2]=1;
	for(int i=0;i<nb;i++){ // parents always precede children in JKA GLAs; verify below
		mdxaBone_t l; localAt(i,A,ta,&l);
		if(B && w>0){ mdxaBone_t lb; localAt(i,*B,tb,&lb); for(int k=0;k<12;k++) ((float*)&l)[k]=((float*)&l)[k]*(1-w)+((float*)&lb)[k]*w; }
		int par=G.skel[i]->parent;
		Mul34(&fin[i], par<0? &root : &fin[par], &l);
	}
}

static std::vector<mdxaBone_t>* gFin;
static const float (*HBoneFn(void*, int i))[4] { return (*gFin)[i].matrix; }
struct Surf { std::string name; const mdxmSurface_t* s; bool cloth; std::vector<float> xyz, nrm; };

#include <utility>
static std::map<std::string,std::pair<double,long>> gLift; static std::map<std::string,double> gLiftMax; static std::map<std::string,std::pair<double,long>> gRel; static std::map<std::string,double> gJit; static float gPrevDt=0.008f; static FILE* gCsv=nullptr; namespace { } extern int g_mcDbgTether; static std::map<std::string,long> gTeth; static std::map<std::string,double> gJumpMax, gJumpT; static float gPrevOrigin[3]; static std::map<std::string,double> gArmIn, gArmWorst, gThru;
int main(int argc,char**argv){
	auto glm=readFile(argv[1]); G.d=readFile(argv[2]); auto cfg=readFile(argv[3]); cfg.push_back(0);
	G.h=(const mdxaHeader_t*)G.d.data();
	const mdxaSkelOffsets_t* so=(const mdxaSkelOffsets_t*)(G.d.data()+sizeof(mdxaHeader_t));
	for(int i=0;i<G.h->numBones;i++){ G.skel.push_back((const mdxaSkel_t*)(G.d.data()+sizeof(mdxaHeader_t)+so->offsets[i])); G.byName[G.skel[i]->name]=i; }
	const mdxmHeader_t* mh=(const mdxmHeader_t*)glm.data();
	const mdxmHierarchyOffsets_t* ho=(const mdxmHierarchyOffsets_t*)(glm.data()+sizeof(mdxmHeader_t));
	const mdxmLOD_t* lod=(const mdxmLOD_t*)(glm.data()+mh->ofsLODs);
	const mdxmLODSurfOffset_t* lso=(const mdxmLODSurfOffset_t*)((const unsigned char*)lod+sizeof(mdxmLOD_t));
	printf("cloth config entries: %d\n", MC_LoadConfig((const char*)cfg.data()));
	std::vector<Surf> surfs;
	for(int i=0;i<mh->numSurfaces;i++){
		const mdxmSurface_t* s=(const mdxmSurface_t*)((const unsigned char*)lso+lso->offsets[i]);
		const mdxmSurfHierarchy_t* sh=(const mdxmSurfHierarchy_t*)((const unsigned char*)ho+ho->offsets[s->thisSurfaceIndex]);
		Surf S; S.name=sh->name; S.s=s; S.cloth=MC_IsClothSurface(s, mh->name, sh->name);
		S.xyz.resize(s->numVerts*4); S.nrm.resize(s->numVerts*4); surfs.push_back(S);
	}
	for(auto&S:surfs) if(S.cloth) printf("cloth surface: %-12s verts %d\n",S.name.c_str(),S.s->numVerts);

	// timeline (seconds): stand, walk, run, stand, lightning, stand
	Anim STAND={"BOTH_STAND1",44800,1,30}, WALK={"BOTH_WALK1",19881,24,20}, RUN={"BOTH_RUN1",44900,24,40}, LIGHT={"BOTH_FORCE_2HANDEDLIGHTNING_HOLD",45355,21,30};
	// ANIMCFG=<animation.cfg>: take the frames from the skeleton's own animation.cfg
	auto fromCfg=[&](Anim& A){ const char* p=getenv("ANIMCFG"); if(!p) return; FILE* f=fopen(p,"r"); if(!f) return; char nm[128]; int a,n,l; float fps; char line[512];
		while(fgets(line,sizeof line,f)) if(sscanf(line,"%127s %d %d %d %f",nm,&a,&n,&l,&fps)==5 && !strcmp(nm,A.name)){ A.first=a; A.num=n; A.fps=fps; break; } fclose(f); };
	if(getenv("LIGHTNAME")) LIGHT.name=getenv("LIGHTNAME");
	fromCfg(STAND); fromCfg(WALK); fromCfg(RUN); fromCfg(LIGHT);
	struct Seg{ Anim a; float t0,t1; float speed; const char* label; };
	std::vector<Seg> segs={ {STAND,0,1.0f,0,"Idle"},{WALK,1.0f,3.5f,55,"Walk"},{RUN,3.5f,5.5f,200,"Run"},{STAND,5.5f,6.5f,0,"Stop"},{LIGHT,6.5f,8.5f,0,"Lightning"},{STAND,8.5f,10.0f,0,"Settle"} };
	Anim FJUMP={"BOTH_FORCEJUMP1",45104,20,12}, INAIR={"BOTH_FORCEINAIR1",5611,2,20}, LAND={"BOTH_FORCELAND1",5693,15,30};
	bool jumpMode=getenv("JUMP")!=nullptr; float vz=0;
	if(getenv("FIGHT")){
		Anim A3TB={"BOTH_A3_T__B_",424,27,40}, A3LR={"BOTH_A3__L__R",451,13,30}, LUNGE={"BOTH_LUNGE2_B__T_",10023,24,30},
			SPIN={"BOTH_SPINATTACK6",14043,41,20}, PROT={"BOTH_A6_SABERPROTECT",649,81,20}, BACK={"BOTH_ATTACK_BACK",1589,29,20}, A5={"BOTH_A5_TR_BL",560,8,30};
		segs={ {STAND,0,0.8f,0,"Idle"},{A3TB,0.8f,1.5f,40,"Strike down"},{A3LR,1.5f,1.95f,40,"Slash L-R"},{A5,1.95f,2.25f,40,"Slash"},
			{LUNGE,2.25f,3.05f,260,"Lunge"},{STAND,3.05f,3.6f,0,"Recover"},{SPIN,3.6f,5.6f,30,"Spin attack"},{BACK,5.6f,7.0f,0,"Back attack"},
			{PROT,7.0f,8.8f,0,"Saber twirl"},{STAND,8.8f,10.0f,0,"Settle"} };
		fromCfg(A3TB); fromCfg(A3LR); fromCfg(LUNGE); fromCfg(SPIN); fromCfg(PROT); fromCfg(BACK); fromCfg(A5);
		for(auto& sg: segs) for(Anim* an: {&A3TB,&A3LR,&LUNGE,&SPIN,&PROT,&BACK,&A5,&STAND}) if(!strcmp(sg.a.name,an->name)) sg.a=*an;
	}
	if(getenv("IDLE")) segs={ {STAND,0,10.0f,0,"Idle"} };
	if(jumpMode) segs={ {STAND,0,1.0f,0,"Idle"},{RUN,1.0f,2.5f,200,"Run"},{FJUMP,2.5f,2.8f,200,"Jump"},{INAIR,2.8f,4.0f,200,"Air"},{LAND,4.0f,4.6f,60,"Land"},{STAND,4.6f,6.0f,0,"Stand"},{FJUMP,6.0f,6.3f,0,"Jump2"},{INAIR,6.3f,7.4f,0,"Air2"},{LAND,7.4f,8.0f,0,"Land2"},{STAND,8.0f,10.0f,0,"Settle"} };
	const float DUR=10.0f; const float XF=getenv("XF")?atof(getenv("XF")):0.2f;
	const char* colliderNames[][2]={{"pelvis","thoracic"},{"thoracic","cervical"},{"cervical","cranium"},{"lfemurYZ","ltibia"},{"ltibia","ltalus"},{"rfemurYZ","rtibia"},{"rtibia","rtalus"},{"lhumerus","lradius"},{"lradius","lhand"},{"rhumerus","rradius"},{"rradius","rhand"}};
	float colliderR[]={6.0f,4.5f,3.8f,5.0f,3.8f,5.0f,3.8f,2.5f,2.2f,2.5f,2.2f};
	const int NC=11;

	mcParams_t prm={650.f,1.4f,6,1.0f,1.0f,0.0f,2.5f,{0,0,0},0.0f,0.0f}; if(getenv("WIND")) sscanf(getenv("WIND"),"%f %f %f",&prm.wind[0],&prm.wind[1],&prm.wind[2]); if(getenv("GUST")) prm.gust=atof(getenv("GUST")); if(getenv("AD")) prm.airDrag=atof(getenv("AD"));
	prm.xpbd=getenv("XPBD")?atoi(getenv("XPBD")):1; prm.xpbdIters=getenv("XI")?atoi(getenv("XI")):2; prm.frictionScale=getenv("FR")?atof(getenv("FR")):1.0f; prm.floor=0; prm.liftLimit=getenv("LIFT")?atof(getenv("LIFT")):-2.0f;
	if(getenv("WALL")){ prm.numWalls=1; sscanf(getenv("WALL"),"%f %f %f %f %f %f",&prm.wallPt[0][0],&prm.wallPt[0][1],&prm.wallPt[0][2],&prm.wallN[0][0],&prm.wallN[0][1],&prm.wallN[0][2]); }
	prm.sleep=getenv("SLEEP")?atoi(getenv("SLEEP")):1; prm.viewDist=getenv("VD")?atof(getenv("VD")):100.f; prm.lodDist=800; prm.cullDist=3000;
	if(getenv("IN")) prm.inertia=atof(getenv("IN")); if(getenv("IT")) prm.iterations=atoi(getenv("IT"));
	if(getenv("G")) prm.gravity=atof(getenv("G")); if(getenv("DR")) prm.drag=atof(getenv("DR"));
	std::map<std::string,std::vector<float>> o1,o2; std::map<std::string,double> jig, spd; long jn=0;
	vec3_t origin={0,0,0}; vec3_t axis[3]={{1,0,0},{0,1,0},{0,0,1}};
	std::vector<mdxaBone_t> fin;
	if(getenv("CSV")) gCsv=fopen(getenv("CSV"),"w");
	FILE* out=fopen(argc>4?argv[4]:"rec.bin","wb");
	int recEvery=5, nrec=0;   // 125 fps engine frames, record every 5th = 25 fps
	// measurements
	std::map<std::string,double> maxStretch, maxDisp, sumDisp, minClear, minClearRigid; std::map<std::string,long> cnt; long nan=0; std::map<std::string,std::vector<float>> stretchS; double applyMs=0;

	// frame timing: FPS env (default 125) and JIT=1 for uneven frame times like a real game
	const float fps=getenv("FPS")?atof(getenv("FPS")):125.f; const bool jit=getenv("JIT")!=nullptr;
	double tAcc=0; int ms=1000; unsigned rng=12345; float fdt=1.f/fps; float tPrev=0;
	for(int frame=0; tAcc<DUR; frame++){
		float t=(float)tAcc; float jf=1.f; if(jit){ rng=rng*1103515245u+12345u; jf=0.6f+0.8f*((rng>>16)&1023)/1023.f; }
		double step=jf/fps; if(getenv("HITCH") && frame%97==50) step+=0.9; tAcc+=step; int msN=1000+(int)lrint(tAcc*1000.0); fdt=(msN-ms)*0.001f; if(frame==0) fdt=1.f/fps; ms=msN;
		int si=0; while(si+1<(int)segs.size() && t>=segs[si+1].t0) si++;
		const Seg& S=segs[si]; float speed=S.speed; const Anim* B=nullptr; float w=0, tb=0;
		if(si>0 && t-S.t0<XF){ B=&segs[si-1].a; w=1-(t-S.t0)/XF; tb=t-segs[si-1].t0; speed=speed*(1-w)+segs[si-1].speed*w; }
		evalBones(S.a,t-S.t0,B,tb,w,fin);
		if(getenv("DEGEN") && t>3.0f && t<3.5f) for(auto& M:fin) for(int r=0;r<3;r++) for(int c=0;c<4;c++) M.matrix[r][c]=(c==3)?M.matrix[r][c]:0.0f;   // collapsed (zero-scale) bones
		if(getenv("NANPOSE") && t>3.0f && t<3.1f) fin[5].matrix[0][0]=NAN;
		if(getenv("TELEPORT") && tPrev<4.0f && t>=4.0f) origin[0]+=5000.f;
		origin[1]-= speed*fdt;      // model faces -y in bind pose
		if(jumpMode){ if(!getenv("NOVZ") && ((tPrev<2.6f&&t>=2.6f)||(tPrev<6.1f&&t>=6.1f))) vz=560.f; if(vz!=0||origin[2]>0){ vz-=800.f*fdt; origin[2]+=vz*fdt; if(origin[2]<=0){origin[2]=0; vz=0;} } }
		// colliders in world space
		mcCapsule_t caps[NC+2]; int NCA=NC;
		for(int c=0;c<NC;c++){ for(int e=0;e<2;e++){ int bi=G.byName[colliderNames[c][e]]; const mdxaBone_t& M=fin[bi]; const mdxaBone_t& BP=G.skel[bi]->BasePoseMat;
			float bp[3]={BP.matrix[0][3],BP.matrix[1][3],BP.matrix[2][3]}; float* dst=e?caps[c].b:caps[c].a;
			for(int k=0;k<3;k++) dst[k]=origin[k]+M.matrix[k][0]*bp[0]+M.matrix[k][1]*bp[1]+M.matrix[k][2]*bp[2]+M.matrix[k][3]; } caps[c].r=colliderR[c]; caps[c].hard=(c>=7 && !getenv("SOFTARMS"))?1:0; }
		if(!getenv("NOFIST")) for(int c: {8,10}){ float d[3],L=0; for(int k=0;k<3;k++){d[k]=caps[c].b[k]-caps[c].a[k]; L+=d[k]*d[k];} L=sqrt(L); if(L<0.1f) continue; for(int k=0;k<3;k++){ caps[NCA].a[k]=caps[c].b[k]; caps[NCA].b[k]=caps[c].b[k]+d[k]/L*4.5f; } caps[NCA].r=2.8f; caps[NCA].hard=1; NCA++; }
		for(auto& Sf:surfs){
			const mdxmSurface_t* s=Sf.s; const mdxmVertex_t* v=(const mdxmVertex_t*)((const unsigned char*)s+s->ofsVerts);
			const int* refs=(const int*)((const unsigned char*)s+s->ofsBoneReferences);
			if(!Sf.cloth) continue;
			for(int i=0;i<s->numVerts;i++){
				int nw=G2_GetVertWeights(&v[i]); float tot=0; float* x=&Sf.xyz[i*4]; float* n=&Sf.nrm[i*4]; x[0]=x[1]=x[2]=n[0]=n[1]=n[2]=0;
				for(int k=0;k<nw;k++){ float wt=G2_GetVertBoneWeight(&v[i],k,tot,nw); const mdxaBone_t& M=fin[refs[G2_GetVertBoneIndex(&v[i],k)]];
					for(int r=0;r<3;r++){ x[r]+=wt*(M.matrix[r][0]*v[i].vertCoords[0]+M.matrix[r][1]*v[i].vertCoords[1]+M.matrix[r][2]*v[i].vertCoords[2]+M.matrix[r][3]);
						n[r]+=wt*(M.matrix[r][0]*v[i].normal[0]+M.matrix[r][1]*v[i].normal[1]+M.matrix[r][2]*v[i].normal[2]); } }
			}
			std::vector<float> animated=Sf.xyz; (void)0;
			auto t0=std::chrono::high_resolution_clock::now();
			static std::vector<int> cls; if(cls.empty()){ cls.assign(G.skel.size(),-1); for(const char* nm: {"pelvis","lower_lumbar","upper_lumbar","thoracic","cervical","cranium","lfemurYZ","lfemurX","ltibia","ltalus","rfemurYZ","rfemurX","rtibia","rtalus"}){ auto it=G.byName.find(nm); if(it!=G.byName.end()) cls[it->second]=0; } }
			mcCapsule_t all[160]; int NALL=0; gFin=&fin;
			int nf= getenv("BODYFIT") ? MC_BodyShapes(s,cls.data(),(int)cls.size(),HBoneFn,nullptr,origin,axis,all,120) : 0;
			if(nf>0){ NALL=nf; const bool keep=atoi(getenv("BODYFIT"))==2; for(int c=0;c<NCA;c++) if(keep||caps[c].hard) all[NALL++]=caps[c]; } else { for(int c=0;c<NCA;c++) all[NALL++]=caps[c]; }
			if(getenv("FITDUMP") && frame==0 && nf){ for(int c=0;c<nf;c++) printf("FIT %d a %.1f %.1f %.1f b %.1f %.1f %.1f r %.1f\n",c,all[c].a[0],all[c].a[1],all[c].a[2],all[c].b[0],all[c].b[1],all[c].b[2],all[c].r); }
			MC_Apply(s,(void*)0x1234,ms,origin,axis,all,NALL,&prm,HBoneFn,nullptr,(float(*)[4])Sf.xyz.data(),(float(*)[4])Sf.nrm.data());
			applyMs+=std::chrono::duration<double,std::milli>(std::chrono::high_resolution_clock::now()-t0).count();
			// measure
			for(int i=0;i<s->numVerts;i++){ for(int k=0;k<3;k++) if(!std::isfinite(Sf.xyz[i*4+k])) nan++;
				float dx=Sf.xyz[i*4]-animated[i*4], dy=Sf.xyz[i*4+1]-animated[i*4+1], dz=Sf.xyz[i*4+2]-animated[i*4+2]; double dd=sqrt(dx*dx+dy*dy+dz*dz);
				maxDisp[Sf.name]=std::max(maxDisp[Sf.name],dd); { static std::map<std::string,int> dummy; } gLift[(vz<0?"FALL":(vz>0?"RISE":S.label))].first+=dz; gLift[(vz<0?"FALL":(vz>0?"RISE":S.label))].second++; gLiftMax[(vz<0?"FALL":(vz>0?"RISE":S.label))]=std::max(gLiftMax[(vz<0?"FALL":(vz>0?"RISE":S.label))],(double)dz); sumDisp[Sf.name]+=dd; cnt[Sf.name]++;
				if(dd>0.01 && frame>20){ float w[3]={Sf.xyz[i*4]+origin[0],Sf.xyz[i*4+1]+origin[1],Sf.xyz[i*4+2]+origin[2]};
					double mc=1e9; for(int c=0;c<NC;c++){ float ab[3],ap[3]; for(int k=0;k<3;k++){ab[k]=caps[c].b[k]-caps[c].a[k]; ap[k]=w[k]-caps[c].a[k];} float L=ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2]; float tt=L>1e-6f?(ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2])/L:0; tt=std::min(1.f,std::max(0.f,tt));
						float q[3]; for(int k=0;k<3;k++) q[k]=w[k]-(caps[c].a[k]+ab[k]*tt); double dist=sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2])-caps[c].r; mc=std::min(mc,dist); }
					if(!minClear.count(Sf.name)) minClear[Sf.name]=1e9; minClear[Sf.name]=std::min(minClear[Sf.name],mc);
					float wa[3]={animated[i*4]+origin[0],animated[i*4+1]+origin[1],animated[i*4+2]+origin[2]}; double mr=1e9;
					for(int c=0;c<NC;c++){ float ab[3],ap[3]; for(int k=0;k<3;k++){ab[k]=caps[c].b[k]-caps[c].a[k]; ap[k]=wa[k]-caps[c].a[k];} float L=ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2]; float tt=L>1e-6f?(ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2])/L:0; tt=std::min(1.f,std::max(0.f,tt));
						float q[3]; for(int k=0;k<3;k++) q[k]=wa[k]-(caps[c].a[k]+ab[k]*tt); mr=std::min(mr,sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2])-caps[c].r); }
					if(!minClearRigid.count(Sf.name)) minClearRigid[Sf.name]=1e9; minClearRigid[Sf.name]=std::min(minClearRigid[Sf.name],mr); }
			}
			{ static std::map<std::string,std::vector<float>> prevW; static float po[3]; auto& PW=prevW[Sf.name]; std::vector<float> W(s->numVerts*3); for(int i=0;i<s->numVerts;i++) for(int k=0;k<3;k++) W[i*3+k]=Sf.xyz[i*4+k]+origin[k];
			  if(PW.size()==W.size()&&frame>30){ double mx=0; int mi=0; for(int i=0;i<s->numVerts;i++){ if(v[i].vertCoords[2]>(getenv("ZFREE")?atof(getenv("ZFREE")):35.f)) continue; double d2=0; for(int k=0;k<3;k++){ double d=W[i*3+k]-PW[i*3+k]-(origin[k]-gPrevOrigin[k]); d2+=d*d;} if(d2>mx){mx=d2;mi=i;} } mx=sqrt(mx)/fdt; if(mx>gJumpMax[S.label]){gJumpMax[S.label]=mx; gJumpT[S.label]=t;} }
			  PW=W; }
			{ auto&A=o1[Sf.name]; auto&B=o2[Sf.name]; std::vector<float> o(s->numVerts*3); for(int i=0;i<s->numVerts;i++) for(int k=0;k<3;k++) o[i*3+k]=Sf.xyz[i*4+k]-animated[i*4+k];
			  if(B.size()&&frame>30){ double j=0,v=0; for(size_t q=0;q<o.size();q+=3){ double a2=0,b2=0; for(int k=0;k<3;k++){ double vr=(o[q+k]-A[q+k])/fdt, vp=(A[q+k]-B[q+k])/gPrevDt; a2+=vr*vr; b2+=(vr-vp)*(vr-vp);} v+=sqrt(a2); j+=sqrt(b2);} int nv=o.size()/3;
			    gRel[S.label].first+=v/nv; gTeth[S.label]+=g_mcDbgTether; g_mcDbgTether=0; gRel[S.label].second++; gJit[S.label]+=j/nv; if(gCsv) fprintf(gCsv,"%.4f,%s,%.2f,%.2f\n",t,S.label,v/nv,j/nv); }
			  B=A; A=o; gPrevDt=fdt; }
			{ double worst=0; long inside=0; for(int i=0;i<s->numVerts;i++){ if(v[i].vertCoords[2]>40.f) continue; float w[3]={Sf.xyz[i*4]+origin[0],Sf.xyz[i*4+1]+origin[1],Sf.xyz[i*4+2]+origin[2]};
				for(int c=7;c<NCA;c++){ float ab[3],ap[3]; for(int k=0;k<3;k++){ab[k]=caps[c].b[k]-caps[c].a[k]; ap[k]=w[k]-caps[c].a[k];} float L=ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2]; float tt=L>1e-6f?(ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2])/L:0; tt=std::min(1.f,std::max(0.f,tt));
					double q2=0; for(int k=0;k<3;k++){ double q=w[k]-(caps[c].a[k]+ab[k]*tt); q2+=q*q;} double pen=caps[c].r-sqrt(q2); if(pen>0.5){ inside++; worst=std::max(worst,pen);} } }
			  gArmIn[S.label]+=inside; gArmWorst[S.label]=std::max(gArmWorst[S.label],worst); }
			// wrapped through an arm: the arm lies between where the animation puts a cloth point and where it is
			{ long thru=0; for(int i=0;i<s->numVerts;i++){ float w[3]={Sf.xyz[i*4]+origin[0],Sf.xyz[i*4+1]+origin[1],Sf.xyz[i*4+2]+origin[2]}, an[3]={animated[i*4]+origin[0],animated[i*4+1]+origin[1],animated[i*4+2]+origin[2]};
				float dd=0; for(int k=0;k<3;k++) dd+=(w[k]-an[k])*(w[k]-an[k]); if(dd<4.f) continue;
				for(int c=7;c<NCA;c++){ // closest approach of segment an-w to the capsule axis, sampled
					double best=1e9; for(int t=1;t<16;t++){ float q[3]; for(int k=0;k<3;k++) q[k]=an[k]+(w[k]-an[k])*t/16.f; float ab[3],ap[3]; for(int k=0;k<3;k++){ab[k]=caps[c].b[k]-caps[c].a[k]; ap[k]=q[k]-caps[c].a[k];} float L=ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2]; float tt=L>1e-6f?(ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2])/L:0; tt=std::min(1.f,std::max(0.f,tt)); double d2=0; for(int k=0;k<3;k++){ double z=q[k]-(caps[c].a[k]+ab[k]*tt); d2+=z*z;} best=std::min(best,d2); }
					if(best<caps[c].r*caps[c].r*0.25){ thru++; break; } } }
			  gThru[S.label]+=thru; }
			// structural stretch vs animated edge lengths
			const int* tri=(const int*)((const unsigned char*)s+s->ofsTriangles);
			for(int tI=0;tI<s->numTriangles;tI++) for(int e=0;e<3;e++){ int a=tri[tI*3+e], b=tri[tI*3+(e+1)%3];
				double la=0, lb=0; for(int k=0;k<3;k++){ double d1=Sf.xyz[a*4+k]-Sf.xyz[b*4+k], d2=animated[a*4+k]-animated[b*4+k]; la+=d1*d1; lb+=d2*d2; }
				if(lb>0.25){ double r=sqrt(la/lb); maxStretch[Sf.name]=std::max(maxStretch[Sf.name], r); if(frame>20) stretchS[Sf.name].push_back((float)r); } }
		}
		if(frame%recEvery==0){
			// record: time, origin, segment, all bone matrices, cloth surface positions (model space)
			float hdr[5]={t,origin[0],origin[1],origin[2],(float)si}; fwrite(hdr,4,5,out);
			for(int b=0;b<G.h->numBones;b++) fwrite(&fin[b],4,12,out);
			for(auto& Sf:surfs) if(Sf.cloth) for(int i=0;i<Sf.s->numVerts;i++) for(int k=0;k<3;k++){ float v=Sf.xyz[i*4+k]*64.0f; if(v>32767)v=32767; if(v<-32768)v=-32768; short q=(short)lrintf(v); fwrite(&q,2,1,out); }
			nrec++;
		}
		if(getenv("BLAST") && tPrev<1.5f && t>=1.5f){ float c[3]={origin[0]+60,origin[1],origin[2]+30}; MC_Blast(c,atof(getenv("BLAST")),200); }
		MC_Prune(ms); tPrev=t; for(int k=0;k<3;k++) gPrevOrigin[k]=origin[k];
	}
	fclose(out);
	printf("recorded %d frames, NaN/inf values: %ld\n",nrec,nan);
	printf("MC_Apply time: %.3f ms per engine frame for all cloth surfaces\n", applyMs/(DUR*125));
	{ mcStats_t st; MC_TakeStats(&st); printf("stats: sims %d steps %d slept %d lod %d culled %d\n", st.simulated, st.steps, st.sleepingSteps, st.lod, st.culled); }
	for(auto& kv:stretchS){ auto v=kv.second; std::sort(v.begin(),v.end()); printf("%-12s edge stretch median %.3fx  99th %.2fx  99.9th %.2fx  max %.2fx | clearance cloth %+.2f vs rigid animation %+.2f\n", kv.first.c_str(), v[v.size()/2], v[(size_t)(v.size()*.99)], v[(size_t)(v.size()*.999)], v.back(), minClear[kv.first], minClearRigid[kv.first]); }
	for(auto& kv:maxDisp) printf("%-12s max move from animation %5.1f  avg %4.2f  | max edge stretch vs animated %4.2fx | min clearance to body %+.2f\n",
		kv.first.c_str(), kv.second, sumDisp[kv.first]/cnt[kv.first], maxStretch[kv.first], minClear.count(kv.first)?minClear[kv.first]:99.0);
	for(auto&kv:jig) printf("JIG %-11s jiggle %.4f  speed %.4f\n",kv.first.c_str(),kv.second/jn*1000,spd[kv.first]/jn*1000);
	for(auto&kv:gRel) printf("MOTION %-7s cloth speed vs body %6.1f u/s   frame-to-frame change %6.1f u/s  tether hits/frame %.1f\n",kv.first.c_str(),kv.second.first/kv.second.second,gJit[kv.first]/kv.second.second,(double)gTeth[kv.first]/kv.second.second);
	for(auto&kv:gJumpMax) printf("PEAK %-7s fastest cloth vertex (body travel removed) %7.0f u/s at t=%.2f\n",kv.first.c_str(),kv.second,gJumpT[kv.first]);
	for(auto&kv:gThru) printf("THRU %-7s cloth points wrapped through an arm per frame %.2f\n",kv.first.c_str(),kv.second/std::max(1.0,gRel[kv.first].second+0.0));
	for(auto&kv:gArmIn) printf("ARMS %-7s cloth verts inside arms/hands per frame %.1f, deepest %.1f units\n",kv.first.c_str(),kv.second/std::max(1.0,gRel[kv.first].second+0.0),gArmWorst[kv.first]);
	for(auto&kv:gLift) printf("LIFT %-7s avg dz %+6.2f  max %+6.2f\n",kv.first.c_str(),kv.second.first/kv.second.second,gLiftMax[kv.first]);
	return 0;
}

#include <cstdarg>
void QDECL Com_Error(int level, const char* fmt, ...){ va_list a; va_start(a,fmt); vfprintf(stderr,fmt,a); va_end(a); exit(1); }
void QDECL Com_Printf(const char* fmt, ...){ va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a); }
