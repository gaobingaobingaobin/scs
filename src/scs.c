/* scs 1.0 */
#include "scs.h"
#include "normalize.h"

#define PRINT_INTERVAL 100
#define CONVERGED_INTERVAL 20
/* scs returns one of the following integers: */
/* (zero should never be returned) */
#define FAILURE -4
#define INDETERMINATE -3
#define INFEASIBLE -2 /* primal infeasible, dual unbounded */
#define UNBOUNDED -1 /* primal unbounded, dual infeasible */
#define SOLVED 1

static idxint _lineLen_;
/* constants and data structures */
static const char* HEADER[] = {
	" Iter ", 
	" pri res ",
	" dua res ",
    " rel gap ",
	" pri obj ",
    " dua obj ",
    "  kappa  ",
	" time (s)",
};

static const idxint HEADER_LEN = 8;

static void updateDualVars(Data * d, Work * w);
static void projectCones(Data * d,Work * w,Cone * k, idxint iter);
static void sety(Data * d, Work * w, Sol * sol);
static void setx(Data * d, Work * w, Sol * sol);
static void sets(Data * d, Work * w, Sol * sol);
static void setSolution(Data * d, Work * w, Sol * sol, Info * info);
static void getInfo(Data * d, Work * w, Sol * sol, Info * info);
static void printSummary(idxint i, struct residuals *r);
static void printHeader(Data * d, Work * w, Cone * k);
static void printFooter(Data * d, Info * info); 
static void freeWork(Work * w);
static void projectLinSys(Data * d,Work * w, idxint iter);
static Work * initWork(Data * d, Cone * k, Sol * sol);
static idxint converged(Data * d, Work * w, struct residuals * r, idxint iter);
static idxint exactConverged(Data * d, Work * w, struct residuals * r);
static idxint validate(Data * d, Cone * k);
static void failureDefaultReturn(Data * d, Sol * sol, Info * info);
static void warmStartVars(Data * d, Work * w, Sol * sol);
static void coldStartVars(Data * d, Work * w);

idxint privateInitWork(Data * d, Work * w);
char * getLinSysSummary(Info * info);
/* solves [I A';A -I] x = b, stores result in b, s contains warm-start */
void solveLinSys(Data * d, Work * w, pfloat * b, const pfloat * s, idxint iter);
void freePriv(Work * w);

idxint scs(Data * d, Cone * k, Sol * sol, Info * info) {
    Work * w = scs_init(d, k, sol, info);
    if (!w) {
        return FAILURE; /* scs_finish called in initWork */
    }
    scs_solve(w, d, k, sol, info);
    scs_finish(d, w);
    return info->statusVal;
}

Work * scs_init(Data * d, Cone * k, Sol * sol, Info * info) {
    Work * w;
    if(d == NULL || k == NULL) {
		failureDefaultReturn(d, sol, info);
        return NULL;
	}
    #ifndef NOVALIDATE
    if (validate(d,k) < 0) {
        failureDefaultReturn(d, sol, info);
        return NULL;
    }
    #endif
   	w = initWork(d,k,sol); /* pass in sol for warm-starting */
	if (!w) {
        failureDefaultReturn(d, sol, info);
        return NULL;
    }
    return w;
}

void scs_finish(Data * d, Work * w){
    if(d->NORMALIZE) unNormalizeA(d,w);
    finishCone();
    freePriv(w);
    freeWork(w);
}

void updateWork(Data * d, Work * w, Sol * sol) {
    /* before normalization */
    w->nm_b = calcNorm(d->b, d->m);
    w->nm_c = calcNorm(d->c, d->n);
	if(d->NORMALIZE) normalizeBC(d,w);
    if (d->WARM_START) {
        warmStartVars(d,w,sol);
    } else {
        coldStartVars(d,w);
    }
    memcpy(w->h,d->c,d->n*sizeof(pfloat));
	memcpy(&(w->h[d->n]),d->b,d->m*sizeof(pfloat));
	memcpy(w->g,w->h,(w->l-1)*sizeof(pfloat));
    solveLinSys(d,w,w->g, NULL, -1);
    scaleArray(&(w->g[d->n]),-1,d->m);
	w->gTh = innerProd(w->h, w->g, w->l-1); 
}

idxint scs_solve(Work * w, Data * d, Cone * k, Sol * sol, Info * info)
{
    idxint i;
    struct residuals r;
    tic();
    info->statusVal = 0; /* not yet converged */
    updateWork(d, w, sol);
    if(d->VERBOSE) printHeader(d, w, k);
    /* scs: */
	for (i=0; i < d->MAX_ITERS; ++i) {
		memcpy(w->u_prev, w->u, w->l*sizeof(pfloat));
		
		projectLinSys(d,w,i);
		projectCones(d,w,k,i);
		updateDualVars(d,w);
	    
        info->statusVal = converged(d,w,&r,i);
		if (info->statusVal != 0) break;

		if (i % PRINT_INTERVAL == 0){
			if (d->VERBOSE) printSummary(i,&r);
		}
	}
	if(d->VERBOSE) printSummary(i,&r);
	setSolution(d,w,sol,info);
    info->iter = i;
	getInfo(d,w,sol,info);
    if(d->VERBOSE) printFooter(d, info);
    /* un-normalize sol, b, c but not A */
    if(d->NORMALIZE) unNormalizeSolBC(d,w,sol);
    return info->statusVal;
}

static idxint validate(Data * d, Cone * k) {
    idxint i, rMax, Anz;
    if (!d->Ax || !d->Ai || !d->Ap || !d->b || !d->c) {
        scs_printf("data incompletely specified\n");
        return -1;
    }
    if (d->m <= 0 || d->n <= 0) {
        scs_printf("m and n must both be greater than 0\n");
        return -1;
    }
    if (d->m < d->n) {
        scs_printf("m must be greater than or equal to n\n");
        return -1;
    }
    for (i = 0 ; i < d->n ; ++i) {
        if(d->Ap[i] >= d->Ap[i+1]) {
            scs_printf("Ap not strictly increasing\n");
            return -1;
        }
    }
    Anz = d->Ap[d->n];
    if (((pfloat) Anz / d->m > d->n) || (Anz <= 0)) {
        scs_printf("Anz (nonzeros in A) = %i, outside of valid range\n", (int) Anz);
        return -1;
    }
    rMax = 0;
    for (i = 0 ; i < Anz ; ++i) {
        if(d->Ai[i] > rMax) rMax = d->Ai[i];
    }
    if (rMax > d->m - 1) {
        scs_printf("number of rows in A inconsistent with input dimension\n");
        return -1;
    }
    if (validateCones(k) < 0) {
        scs_printf("invalid cone dimensions\n");
        return -1;
    }
    if (getFullConeDims(k) != d->m) {
        scs_printf("cone dimensions %i not equal to num rows in A = m = %i\n", (int) getFullConeDims(k), (int) d->m );
        return -1;
    }
    if (d->MAX_ITERS < 0) {
        scs_printf("MAX_ITERS must be positive\n");
        return -1;
    }
    if (d->EPS < 0) {
        scs_printf("EPS tolerance must be positive\n");
        return -1;
    }
    if (d->ALPHA <= 0 || d->ALPHA >= 2) {
        scs_printf("ALPHA must be in (0,2)\n");
        return -1;
    }
    if (d->RHO_X < 0) {
        scs_printf("RHO_X must be positive (1e-3 works well).\n");
        return -1;
    }
    return 0;
}

static void failureDefaultReturn(Data * d, Sol * sol, Info * info){
    info->relGap = NAN;
    info->resPri = NAN;
    info->resDual = NAN;
    info->pobj = NAN;
    info->dobj = NAN;
    info->iter = -1;
    info->statusVal = FAILURE;
    info->time = NAN;
    strcpy(info->status,"Failure");
    sol->x = scs_malloc(sizeof(pfloat)*d->n);
    scaleArray(sol->x,NAN,d->n);
    sol->y = scs_malloc(sizeof(pfloat)*d->m);
    scaleArray(sol->y,NAN,d->m);
    sol->s = scs_malloc(sizeof(pfloat)*d->m);
    scaleArray(sol->s,NAN,d->m);
    scs_printf("FAILURE\n");
}

static idxint converged(Data * d, Work * w, struct residuals * r, idxint iter){
    /* approximate convergence check: */
    /* abs to prevent negative stopping tol */
    /*
    pfloat tau = ABS(w->u[w->l-1]);     
    pfloat kap = ABS(w->v[w->l-1]);
    r->resPri = calcNormDiff(w->u, w->u_t, w->l);
    r->resDual = calcNormDiff(w->u, w->u_prev, w->l);
    r->tau = tau;
    r->kap = kap;
    */
    /*
    if (MIN(tau,kap)/MAX(tau,kap) < 1e-6 && MAX(r->resPri, r->resDual) < d->EPS*(tau+kap)){
        return 1;
    }
    */
    if (iter % CONVERGED_INTERVAL == 0) {
        return exactConverged(d,w,r);
    }
    return 0;
}

static pfloat calcPrimalResid(Data * d, Work * w, pfloat * x, pfloat * s, pfloat tau, pfloat *nmAxs) {
    idxint i;
    pfloat pres = 0, scale, * pr = w->pr, *D = w->D;
    *nmAxs = 0;
    memset(pr, 0, d->m * sizeof(pfloat));
    accumByA(d,x,pr);
    addScaledArray(pr,s,d->m,1.0);  /* pr = Ax + s */
    for (i = 0; i < d->m; ++i) {
        scale = d->NORMALIZE ? D[i]/(w->sc_b * w->scale) : 1;
        scale = scale * scale;
        *nmAxs += (pr[i] * pr[i]) * scale;
        pres += (pr[i] - d->b[i] * tau) * (pr[i] - d->b[i] * tau) * scale;
    }
    *nmAxs = sqrt(*nmAxs);
    return sqrt(pres); /* norm(Ax + s - b * tau) */
}

static pfloat calcDualResid(Data * d, Work * w, pfloat * y, pfloat tau, pfloat *nmATy){
    idxint i;
    pfloat dres = 0, scale, * dr = w->dr, *E = w->E;
    *nmATy = 0;
    memset(dr, 0, d->n * sizeof(pfloat));
    accumByAtrans(d,y,dr); /* dr = A'y */
    for (i = 0; i < d->n; ++i){
        scale = d->NORMALIZE ? E[i]/(w->sc_c * w->scale) : 1;
        scale = scale * scale;
        *nmATy += (dr[i] * dr[i]) * scale;
        dres += (dr[i] + d->c[i] * tau) * (dr[i] + d->c[i] * tau) * scale;
    }
    *nmATy = sqrt(*nmATy);
    return sqrt(dres); /* norm(A'y + c * tau) */
}

static pfloat fastCalcPrimalResid(Data * d, Work * w, pfloat * nmAxs) {
    idxint i;
    pfloat pres = 0, scale, * pr = w->pr, *D = w->D, tau = ABS(w->u[w->l-1]);
    *nmAxs = 0;
    memcpy(pr, &(w->u[d->n]), d->m * sizeof(pfloat)); /* overwrite pr */
    addScaledArray(pr, &(w->u_prev[d->n]), d->m, d->ALPHA-2);
    addScaledArray(pr, &(w->u_t[d->n]), d->m, 1-d->ALPHA);
    addScaledArray(pr, d->b, d->m, w->u_t[w->l-1]); /* pr = Ax + s */
    for (i = 0; i < d->m; ++i) {
        scale = d->NORMALIZE ? D[i]/(w->sc_b * w->scale) : 1;
        scale = scale * scale;
        *nmAxs += (pr[i] * pr[i]) * scale;
        pres += (pr[i] - d->b[i] * tau) * (pr[i] - d->b[i] * tau) * scale;
    }
    *nmAxs = sqrt(*nmAxs);
    return sqrt(pres); /* norm(Ax + s - b * tau) */
} 

static idxint exactConverged(Data * d, Work * w, struct residuals * r){
    pfloat nmpr, nmdr, tau, kap, *x, *y, cTx, nmAxs, bTy, nmATy;
    x = w->u;
    y = &(w->u[d->n]);
    tau = ABS(w->u[w->l-1]);
    kap = ABS(w->v[w->l-1]);
    r->tau = tau;
    r->kap = kap;
    
    /* requires mult by A:
    nmpr = calcPrimalResid(d, w, w->u, &(w->v[d->n]), ABS(w->u[w->l-1]), &nmAxs);
    */

    /* does not require mult by A: */
    nmpr = fastCalcPrimalResid(d, w, &nmAxs); 
    cTx = innerProd(x,d->c,d->n);
    if (d->NORMALIZE) {
        kap /= (w->scale * w->sc_c * w->sc_b);
        cTx /= (w->scale * w->sc_c * w->sc_b);
    }

    r->resPri = cTx < 0 ? w->nm_c * nmAxs / -cTx : NAN;
    /*scs_printf("unbounded cert: %4e\n", w->nm_c * nmAxs / (1+w->nm_b) / -cTx); */
    if (r->resPri < d->EPS) {
        return UNBOUNDED;
    }

    nmdr = calcDualResid(d, w, y, tau, &nmATy);
    bTy = innerProd(y,d->b,d->m);
    if (d->NORMALIZE) {
        bTy /= (w->scale * w->sc_c * w->sc_b);
    }

    r->resDual = bTy < 0 ? w->nm_b * nmATy / -bTy : NAN;
    /*scs_printf("infeas cert: %4e\n", w->nm_b * nmATy / (1+w->nm_c) /  - bTy ); */
    if (r->resDual < d->EPS) {
        return INFEASIBLE;
    }
    r->relGap = NAN;

    if (tau > kap) {
        pfloat rpri = nmpr / (1+w->nm_b) / tau;
        pfloat rdua = nmdr / (1+w->nm_c) / tau;
        pfloat gap = ABS(cTx + bTy) / (tau + ABS(cTx) + ABS(bTy));

        r->resPri = rpri;
        r->resDual = rdua;
        r->relGap = gap;
        r->cTx = cTx / tau;
        r->bTy = bTy / tau;
        /* scs_printf("primal resid: %4e, dual resid %4e, pobj %4e, dobj %4e, gap %4e\n", rpri,rdua,cTx,-bTy,gap); */
        /* scs_printf("primal resid: %4e, dual resid %4e, gap %4e\n",rpri,rdua,gap); */
        if (MAX(MAX(rpri,rdua),gap) < d->EPS) {
            return SOLVED;
        }
    } else {
        r->cTx = NAN;
        r->bTy = NAN;
    }
    return 0;
}

static void getInfo(Data * d, Work * w, Sol * sol, Info * info){
    pfloat cTx, bTy, nmAxs, nmATy, nmpr, nmdr;
    pfloat * x = sol->x, * y = sol->y, * s = sol->s;
    
    /* unNomalized */
    nmpr = calcPrimalResid(d, w, x, s, 1, &nmAxs); /* pr = Ax + s - b */
    nmdr = calcDualResid(d, w, y, 1, &nmATy); /* dr = A'y + c */
 
    cTx = innerProd(x,d->c,d->n);
    bTy = innerProd(y,d->b,d->m);
    if (d->NORMALIZE) {
        cTx /= (w->scale * w->sc_c * w->sc_b);
        bTy /= (w->scale * w->sc_c * w->sc_b);
    }
    info->pobj = cTx;
    info->dobj = -bTy;
    if (info->statusVal == SOLVED){
        info->relGap = ABS(cTx + bTy) / (1 + ABS(cTx) + ABS(bTy));
        info->resPri = nmpr / (1 + w->nm_b);
        info->resDual = nmdr / (1+ w->nm_c);
    } else {
        if (info->statusVal == UNBOUNDED) {    
            info->dobj = NAN;
            info->relGap = NAN;
            info->resPri = w->nm_c * nmAxs / -cTx ;
            info->resDual = NAN;
            scaleArray(x,-1/cTx,d->n);
            scaleArray(s,-1/cTx,d->m);
            info->pobj = -1;
        }
        else {
            info->pobj = NAN;
            info->relGap = NAN;
            info->resPri = NAN;
            info->resDual = w->nm_b * nmATy / -bTy ;
            scaleArray(y,-1/bTy,d->m);
            info->dobj = -1;
        }
    }
    info->time = tocq();
}

static void warmStartVars(Data * d, Work * w, Sol * sol) {
    memset(w->v, 0, d->n*sizeof(pfloat));
    memcpy(w->u,sol->x,d->n*sizeof(pfloat));
    memcpy(&(w->u[d->n]),sol->y,d->m*sizeof(pfloat));
    memcpy(&(w->v[d->n]),sol->s,d->m*sizeof(pfloat));
    w->u[w->l-1] = 1.0;
    w->v[w->l-1] = 0.0;
    if (d->NORMALIZE) normalizeWarmStart(d, w);
}

static void coldStartVars(Data * d, Work * w) {
    memset(w->u, 0, w->l*sizeof(pfloat));
    memset(w->v, 0, w->l*sizeof(pfloat));
    w->u[w->l-1] = sqrt(w->l);
    w->v[w->l-1] = sqrt(w->l);
}

/* pass in sol for warm-starting */
static Work * initWork(Data *d, Cone * k, Sol * sol) {
    idxint status;
	Work * w = scs_malloc(sizeof(Work));
    if(d->NORMALIZE) {
		normalizeA(d,w,k);
	}
	else {
		w->D = NULL;
		w->E = NULL;
		w->meanNormRowA = 0.0; /* unused */
        w->sc_c = 1.0;
		w->sc_b = 1.0;
		w->scale = 1.0;
	}
	w->l = d->n+d->m+1;
	/* allocate workspace: */
    w->u = scs_malloc(w->l * sizeof(pfloat));
    w->v = scs_malloc(w->l * sizeof(pfloat));
	w->u_t = scs_malloc(w->l * sizeof(pfloat));
	w->u_prev = scs_malloc(w->l * sizeof(pfloat));
	w->h = scs_malloc((w->l-1) * sizeof(pfloat));
    w->g = scs_malloc((w->l-1) * sizeof(pfloat));
    w->pr = scs_malloc(d->m * sizeof(pfloat));
    w->dr = scs_malloc(d->n * sizeof(pfloat));

    status = privateInitWork(d, w);
    if (status < 0){
		scs_printf("privateInitWork failure: %i\n",(int) status);
        scs_finish(d, w);
        return NULL;
    }
    status = initCone(k);
    if (status < 0){
        scs_printf("initCone failure: %i\n",(int) status);
        scs_finish(d, w);
        return NULL;
    }
    return w;
}

static void projectLinSys(Data * d,Work * w, idxint iter){
	/* ut = u + v */
	memcpy(w->u_t,w->u,w->l*sizeof(pfloat));
	addScaledArray(w->u_t,w->v,w->l,1.0);
	
	scaleArray(w->u_t,d->RHO_X,d->n);

	addScaledArray(w->u_t,w->h,w->l-1,-w->u_t[w->l-1]);
	addScaledArray(w->u_t, w->h, w->l-1, -innerProd(w->u_t,w->g,w->l-1)/(w->gTh+1));
	scaleArray(&(w->u_t[d->n]),-1,d->m);
	
	solveLinSys(d, w, w->u_t, w->u, iter);
	
	w->u_t[w->l-1] += innerProd(w->u_t,w->h,w->l-1);
}

static void freeWork(Work * w){
	if(w){
        if(w->method) free(w->method); /*called via malloc not mxMalloc */
		if(w->u) scs_free(w->u);
		if(w->v) scs_free(w->v);
		if(w->u_t) scs_free(w->u_t);
		if(w->u_prev) scs_free(w->u_prev);
		if(w->h) scs_free(w->h);
		if(w->g) scs_free(w->g);
		if(w->D) scs_free(w->D);
		if(w->E) scs_free(w->E);
        if(w->pr) scs_free(w->pr);
        if(w->dr) scs_free(w->dr);
		scs_free(w);
	}
}

void printSol(Data * d, Sol * sol, Info * info){
	idxint i;
	scs_printf("%s\n",info->status); 
	if (sol->x != NULL){
		for ( i=0;i<d->n; ++i){
			scs_printf("x[%i] = %4f\n",(int) i, sol->x[i]);
		}
	}
	if (sol->y != NULL){
		for ( i=0;i<d->m; ++i){
			scs_printf("y[%i] = %4f\n", (int) i, sol->y[i]);
		}
	}
}

static void updateDualVars(Data * d, Work * w){
	idxint i;
	/*
	   for(i = 0; i < d->n; ++i) { 
	   w->v[i] += w->u[i] - w->u_t[i]; 
	   }
	 */
	/*for(i = 0; i < w->l; ++i) {  */
	if (ABS(d->ALPHA - 1.0) < 1e-9) {
		/* this is over-step parameter: */
		/*pfloat sig = (1+sqrt(5))/2; */
		pfloat sig = 1.0;
		for(i = d->n; i < w->l; ++i) { 
			w->v[i] += sig*(w->u[i] - w->u_t[i]);
		}
	}
	else {
		/* this does not relax 'x' variable */
		for(i = d->n; i < w->l; ++i) { 
			w->v[i] += (w->u[i] - d->ALPHA*w->u_t[i] - (1.0 - d->ALPHA)*w->u_prev[i]); 
		}
	}
}

static void projectCones(Data *d,Work * w,Cone * k, idxint iter){
	idxint i;
	/* this does not relax 'x' variable */
	for(i = 0; i < d->n; ++i) { 
		w->u[i] = w->u_t[i] - w->v[i];
	}
	/*for(i = 0; i < w->l; ++i){ */
	for(i = d->n; i < w->l; ++i) {
		w->u[i] = d->ALPHA*w->u_t[i] + (1-d->ALPHA)*w->u_prev[i] - w->v[i];
	}
	/* u = [x;y;tau] */
	projCone(&(w->u[d->n]),k,iter);
	if (w->u[w->l-1]<0.0) w->u[w->l-1] = 0.0;
}

static idxint solved(Data * d, Sol * sol, Info * info, pfloat tau){
    strcpy(info->status,"Solved");
    scaleArray(sol->x,1.0/tau,d->n);
    scaleArray(sol->y,1.0/tau,d->m);
    scaleArray(sol->s,1.0/tau,d->m);
    return SOLVED;
}

static idxint indeterminate(Data * d, Sol * sol, Info * info){
    strcpy(info->status, "Indeterminate");
    scaleArray(sol->x,NAN,d->n);
    scaleArray(sol->y,NAN,d->m);
    scaleArray(sol->s,NAN,d->m);
    return INDETERMINATE;
}

static idxint infeasible(Data * d, Sol * sol, Info * info){
    strcpy(info->status,"Infeasible");
    /*scaleArray(sol->y,-1/ip_y,d->m); */
    scaleArray(sol->x,NAN,d->n);
    scaleArray(sol->s,NAN,d->m);
    return INFEASIBLE;
}

static idxint unbounded(Data * d, Sol * sol, Info * info){
    strcpy(info->status,"Unbounded");
    /*scaleArray(sol->x,-1/ip_x,d->n); */
    scaleArray(sol->y,NAN,d->m);
    return UNBOUNDED;
}

static void setSolution(Data * d, Work * w, Sol * sol, Info * info){
    setx(d,w,sol);
    sety(d,w,sol);
    sets(d,w,sol);
    if (info->statusVal == 0 || info->statusVal == SOLVED){
        pfloat tau = w->u[w->l-1];
        pfloat kap = ABS(w->v[w->l-1]);
        if (tau > d->UNDET_TOL && tau > kap){
            info->statusVal = solved(d,sol,info,tau);
        }   
        else{ 
            if (calcNorm(w->u,w->l) < d->UNDET_TOL*sqrt(w->l)){
                info->statusVal = indeterminate(d,sol,info);
            }   
            else {
                pfloat bTy = innerProd(d->b,sol->y,d->m);  
                pfloat cTx = innerProd(d->c,sol->x,d->n);  
                if (bTy < cTx){
                    info->statusVal = infeasible(d,sol,info);
                }   
                else{
                    info->statusVal = unbounded(d,sol,info);
                }
            }
        }
    } else if (info->statusVal == INFEASIBLE) {
        info->statusVal = infeasible(d,sol,info);
    } else {
        info->statusVal = unbounded(d,sol,info);
    }
}

static void sety(Data * d,Work * w, Sol * sol){
	if(!sol->y) sol->y = scs_malloc(sizeof(pfloat)*d->m);
	memcpy(sol->y, &(w->u[d->n]), d->m*sizeof(pfloat));
}

static void sets(Data * d,Work * w, Sol * sol){
    if(!sol->s) sol->s = scs_malloc(sizeof(pfloat)*d->m);
	memcpy(sol->s, &(w->v[d->n]), d->m*sizeof(pfloat));
}

static void setx(Data * d,Work * w, Sol * sol){
	if(!sol->x) sol->x = scs_malloc(sizeof(pfloat)*d->n);
	memcpy(sol->x, w->u, d->n*sizeof(pfloat));
}

static void printSummary(idxint i, struct residuals *r){
    scs_printf("%*i|", (int)strlen(HEADER[0]), (int) i);
	scs_printf(" %*.2e ", (int)strlen(HEADER[1])-1, r->resPri);
	scs_printf(" %*.2e ", (int)strlen(HEADER[2])-1, r->resDual);
	scs_printf(" %*.2e ", (int)strlen(HEADER[3])-1, r->relGap);
    if (r->cTx < 0) {
	    scs_printf("%*.2e ", (int)strlen(HEADER[4])-1, r->cTx);
    } else {
        scs_printf(" %*.2e ", (int)strlen(HEADER[4])-1, r->cTx);
    }
    if (r->bTy >= 0) {
	    scs_printf("%*.2e ", (int)strlen(HEADER[5])-1, -r->bTy);
    } else {
	    scs_printf(" %*.2e ", (int)strlen(HEADER[5])-1, -r->bTy);
    }
    scs_printf(" %*.2e ", (int)strlen(HEADER[6])-1, r->kap);
	scs_printf(" %*.2e ", (int)strlen(HEADER[7])-1, tocq()/1e3);
	scs_printf("\n");
#ifdef MATLAB_MEX_FILE
	mexEvalString("drawnow;");
#endif
}

static void printHeader(Data * d, Work * w, Cone * k) {
	idxint i;  
	char * coneStr = getConeHeader(k);
    _lineLen_ = -1;
    /* scs_printf("size of idxint %lu, size of pfloat %lu\n", sizeof(idxint), sizeof(pfloat)); */ 
	for(i = 0; i < HEADER_LEN; ++i) {
		_lineLen_ += strlen(HEADER[i]) + 1;
	}
	for(i = 0; i < _lineLen_; ++i) {
		scs_printf("-");
	}
	scs_printf("\n\n\tscs v1.0 - Splitting Conic Solver\n\t(c) Brendan O'Donoghue, Stanford University, 2014\n\n");
       	for(i = 0; i < _lineLen_; ++i) {
		scs_printf("-");
	}
    scs_printf("\nmethod: %s\n", w->method);
    scs_printf("EPS = %.2e, ALPHA = %.2f, MAX_ITERS = %i, NORMALIZE = %i\n", d->EPS, d->ALPHA, (int) d->MAX_ITERS, (int) d->NORMALIZE);
	scs_printf("variables n = %i, constraints m = %i, non-zeros in A = %li\n", (int) d->n, (int) d->m, (long) d->Ap[d->n]);
    if(d->WARM_START) scs_printf("using variable warm-starting!\n");

    scs_printf("%s",coneStr);
    free(coneStr);

    for(i = 0; i < _lineLen_; ++i) {
		scs_printf("-");
	}
	scs_printf("\n");
	for(i = 0; i < HEADER_LEN - 1; ++i) {
		scs_printf("%s|", HEADER[i]);
	}
	scs_printf("%s\n", HEADER[HEADER_LEN-1]);
	for(i = 0; i < _lineLen_; ++i) {
		scs_printf("=");
	}
	scs_printf("\n");
}

static void printFooter(Data * d, Info * info) {
	idxint i;
	char * linSysStr = getLinSysSummary(info);
    for(i = 0; i < _lineLen_; ++i) {
		scs_printf("-");
	}
	scs_printf("\nStatus: %s\n",info->status);
	if (info->iter == d->MAX_ITERS) {
		scs_printf("Hit MAX_ITERS, solution may be inaccurate\n"); 
	}
    scs_printf("Time taken: %.4f seconds\n",info->time/1e3);

    if (linSysStr) {
        scs_printf("%s",linSysStr);
        free(linSysStr);
    }

	for(i = 0; i < _lineLen_; ++i) {
		scs_printf("-");
	}
    scs_printf("\n");

    if (info->statusVal == INFEASIBLE) {
        scs_printf("Certificate of primal infeasibility:\n");
        scs_printf("|A'y|_2 * |b|_2 = %.4e\n", info->resDual);
        scs_printf("dist(y, K*) = 0\n");
        scs_printf("b'y = %.4f\n", info->dobj);
    } 
    else if (info->statusVal == UNBOUNDED) {
        scs_printf("Certificate of dual infeasibility:\n");
        scs_printf("|Ax + s|_2 * |c|_2 = %.4e\n", info->resPri);
        scs_printf("dist(s, K) = 0\n");
        scs_printf("c'x = %.4f\n", info->pobj);
    }
    else {
        scs_printf("Error metrics:\n");
        scs_printf("|Ax + s - b|_2 / (1 + |b|_2) = %.4e\n",info->resPri); 
        scs_printf("|A'y + c|_2 / (1 + |c|_2) = %.4e\n",info->resDual);
        scs_printf("|c'x + b'y| / (1 + |c'x| + |b'y|) = %.4e\n", info->relGap); 
        scs_printf("dist(s, K) = 0, dist(y, K*) = 0, s'y = 0\n");
        for(i = 0; i < _lineLen_; ++i) {
            scs_printf("-");
        }
        scs_printf("\n");
        scs_printf("c'x = %.4f, -b'y = %.4f\n",info->pobj, info->dobj);
    }
    for(i = 0; i < _lineLen_; ++i) {
        scs_printf("=");
    }
    scs_printf("\n");
}
