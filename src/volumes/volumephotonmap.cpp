
/*
 * pbrt source code Copyright(c) 1998-2005 Matt Pharr and Greg Humphreys
 *
 * All Rights Reserved.
 * For educational use only; commercial use expressly forbidden.
 * NO WARRANTY, express or implied, for this software.
 * (See file License.txt for complete license)
 * Adapted by Robert Travis from ExPhotonMap and the Single Volume
 * Integrator, provides a unified interface so that both Volume
 * Integrators and Surface integrators (PBRT) have access to the photon maps,
 * meaning only one preprocess. Code adapted from Suwandi and Xu's
 * rendering steam (RS). Preprocess has been modified to trace photons
 * through the volume and record them in the new photon list, volumePhotons.
 * The pieces of code of interest are: TraceThrough and PathMarch, as well
 * as modifications to to Surface_Preprocess and Volume_Li.
 */

// exphotonmap.cpp* -- from PBRT
#include "pbrt.h"
#include "scene.h"
#include "kdtree.h"

struct ClosePhoton;

// Local Declarations
struct Photon {
	Photon(const Point &pp, const Spectrum &wt, const Vector &w)
		: p(pp), alpha(wt), wi(w) {
	}
	Photon() { } // NOBOOK
	Point p;
	Spectrum alpha;
	Vector wi;
};
struct RadiancePhoton {
	RadiancePhoton(const Point &pp, const Normal &nn)
		: p(pp), n(nn), Lo(0.f) {
	}
	RadiancePhoton() { } // NOBOOK
	Point p;
	Normal n;
	Spectrum Lo;
};
struct RadiancePhotonProcess {
	// RadiancePhotonProcess Methods
	RadiancePhotonProcess(const Point &pp, const Normal &nn)
		: p(pp), n(nn) {
		photon = NULL;
	}
	void operator()(const RadiancePhoton &rp,
			float distSquared, float &maxDistSquared) const {
		if (Dot(rp.n, n) > 0) {
			photon = &rp;
			maxDistSquared = distSquared;
		}
	}
	const Point &p;
	const Normal &n;
	mutable const RadiancePhoton *photon;
};
inline float kernel(const Photon *photon, const Point &p,
		float md2) {
//	return 1.f / (md2 * M_PI); // NOBOOK
	float s = (1.f - DistanceSquared(photon->p, p) / md2);
	return 3.f / (md2 * M_PI) * s * s;
}

struct PhotonProcess {
	// PhotonProcess Public Methods
	PhotonProcess(u_int mp, const Point &p);
	void operator()(const Photon &photon, float dist2, float &maxDistSquared) const;
	const Point &p;
	ClosePhoton *photons;
	u_int nLookup;
	mutable u_int foundPhotons;
};
struct ClosePhoton {
	ClosePhoton(const Photon *p = NULL,
	            float md2 = INFINITY) {
		photon = p;
		distanceSquared = md2;
	}
	bool operator<(const ClosePhoton &p2) const {
		return distanceSquared == p2.distanceSquared ? (photon < p2.photon) :
			distanceSquared < p2.distanceSquared;
	}
	const Photon *photon;
	float distanceSquared;
};

PhotonProcess::PhotonProcess(u_int mp, const Point &P)
	: p(P) {
	photons = 0;
	nLookup = mp;
	foundPhotons = 0;
}

//Amalgam of ExPhotonMap and Single volume integrator
class VolumePhotonMap {
public:
	// ExPhotonIntegrator Public Methods
	void Surface_Init(int ncaus, int nindir, int nLookup, int mdepth,
			 float maxdist, bool finalGather, int gatherSamples,
			 float rrt, float ga);
	void Surface_Dispose();
	Spectrum Surface_Li(const Scene *scene, const RayDifferential &ray,
		const Sample *sample, float *alpha) const;
	void Surface_RequestSamples(Sample *sample, const Scene *scene);
	void Surface_Preprocess(const Scene *);

public:
	// SingleScattering Public Methods
	void Volume_Init(int nVolume, float ss) { nVolumePhotons = nVolume; stepSize = ss; }
	void Volume_Dispose() { }
	Spectrum Volume_Transmittance(const Scene *, const Ray &ray,
		const Sample *sample, float *alpha) const;
	void Volume_RequestSamples(Sample *sample, const Scene *scene);
	Spectrum Volume_Li(const Scene *, const RayDifferential &ray, const Sample *sample, float *alpha) const;

private:
	// SingleScattering Private Data
	bool alreadyPreprocessed;
	float stepSize;
	int tauSampleOffset, scatterSampleOffset;

private:
	static inline bool unsuccessful(int needed, int found, int shot) {
		return (found < needed &&
		        (found == 0 || found < shot / 1024));
	}
	static Spectrum Surface_LPhoton(KdTree<Photon, PhotonProcess> *map,
		int nPaths, int nLookup, BSDF *bsdf, const Intersection &isect,
		const Vector &w, float maxDistSquared);

	Spectrum Surface_estimateE(KdTree<Photon, PhotonProcess> *map, int count,
		const Point &p, const Normal &n) const;

	// ExPhotonIntegrator Private Data
	int gatherSampleOffset[2], gatherComponentOffset[2];
        u_int nCausticPhotons, nIndirectPhotons, nVolumePhotons;
	u_int nLookup;
	mutable int specularDepth;
	int maxSpecularDepth;
	float maxDistSquared, rrTreshold;
        bool finalGather;
	float cosGatherAngle;
	int gatherSamples;
	// Declare sample parameters for light source sampling
	int *lightSampleOffset, lightNumOffset;
	int *bsdfSampleOffset, *bsdfComponentOffset;
	int nCausticPaths, nIndirectPaths, nVolumePaths;
	mutable KdTree<Photon, PhotonProcess> *causticMap;
	mutable KdTree<Photon, PhotonProcess> *indirectMap;
	mutable KdTree<Photon, PhotonProcess> *volumeMap;
	mutable KdTree<RadiancePhoton, RadiancePhotonProcess> *radianceMap;

	bool RayMarch(const Scene * scene, Ray & marchRay, Point & loc, float tIntersect, Spectrum & alpha, float & albedo);
	bool TraceThrough(const Scene * scene, RayDifferential & ray, Intersection & isect, Spectrum & alpha, 
		  int shot, bool & volumeDone, vector<Photon>& volumePhotons, ProgressReporter & progress);
};

// ExPhotonIntegrator Method Definitions
Spectrum VolumePhotonMap::Surface_estimateE(
	KdTree<Photon, PhotonProcess> *map, int count,
	const Point &p, const Normal &n) const {
	if (!map) return 0.f;
	// Lookup nearby photons at irradiance computation point
	PhotonProcess proc(nLookup, p);
	proc.photons = (ClosePhoton *)alloca(nLookup *
		sizeof(ClosePhoton));
	float md2 = maxDistSquared;
	map->Lookup(p, proc, md2);
	// Accumulate irradiance value from nearby photons
	ClosePhoton *photons = proc.photons;
	Spectrum E(0.);
	for (u_int i = 0; i < proc.foundPhotons; ++i)
		if (Dot(n, photons[i].photon->wi) > 0.)
			E += photons[i].photon->alpha;
	return E / (float(count) * md2 * M_PI);
}
void PhotonProcess::operator()(const Photon &photon,
		float distSquared, float &maxDistSquared) const {
	// Do usual photon heap management
	static StatsPercentage discarded("Photon Map", "Discarded photons"); // NOBOOK
	discarded.Add(0, 1); // NOBOOK
	if (foundPhotons < nLookup) {
		// Add photon to unordered array of photons
		photons[foundPhotons++] = ClosePhoton(&photon, distSquared);
		if (foundPhotons == nLookup) {
			std::make_heap(&photons[0], &photons[nLookup]);
			maxDistSquared = photons[0].distanceSquared;
		}
	}
	else {
		// Remove most distant photon from heap and add new photon
		discarded.Add(1, 0); // NOBOOK
		std::pop_heap(&photons[0], &photons[nLookup]);
		photons[nLookup-1] = ClosePhoton(&photon, distSquared);
		std::push_heap(&photons[0], &photons[nLookup]);
		maxDistSquared = photons[0].distanceSquared;
	}
}
Spectrum VolumePhotonMap::Surface_LPhoton(
		KdTree<Photon, PhotonProcess> *map,
		int nPaths, int nLookup, BSDF *bsdf,
		const Intersection &isect, const Vector &wo,
		float maxDistSquared) {
	Spectrum L(0.);
	if (!map) return L;
	BxDFType nonSpecular = BxDFType(BSDF_REFLECTION |
		BSDF_TRANSMISSION | BSDF_DIFFUSE | BSDF_GLOSSY);
	if (bsdf->NumComponents(nonSpecular) == 0)
		return L;
	static StatsCounter lookups("Photon Map", "Total lookups"); // NOBOOK
	// Initialize _PhotonProcess_ object, _proc_, for photon map lookups
	PhotonProcess proc(nLookup, isect.dg.p);
	proc.photons =
		(ClosePhoton *)alloca(nLookup * sizeof(ClosePhoton));
	// Do photon map lookup
	++lookups;  // NOBOOK
	map->Lookup(isect.dg.p, proc, maxDistSquared);
	// Accumulate light from nearby photons
	static StatsRatio foundRate("Photon Map", "Photons found per lookup"); // NOBOOK
	foundRate.Add(proc.foundPhotons, 1); // NOBOOK
	// Estimate reflected light from photons
	ClosePhoton *photons = proc.photons;
	int nFound = proc.foundPhotons;
	Normal Nf = Dot(wo, bsdf->dgShading.nn) < 0 ? -bsdf->dgShading.nn :
		bsdf->dgShading.nn;

	if (bsdf->NumComponents(BxDFType(BSDF_REFLECTION |
			BSDF_TRANSMISSION | BSDF_GLOSSY)) > 0) {
		// Compute exitant radiance from photons for glossy surface
		for (int i = 0; i < nFound; ++i) {
			const Photon *p = photons[i].photon;
			BxDFType flag = Dot(Nf, p->wi) > 0.f ?
				BSDF_ALL_REFLECTION : BSDF_ALL_TRANSMISSION;
			float k = kernel(p, isect.dg.p, maxDistSquared);
			L += (k / nPaths) * bsdf->f(wo, p->wi, flag) * p->alpha;
		}
	}
	else {
		// Compute exitant radiance from photons for diffuse surface
		Spectrum Lr(0.), Lt(0.);
		for (int i = 0; i < nFound; ++i) {
			if (Dot(Nf, photons[i].photon->wi) > 0.f) {
				float k = kernel(photons[i].photon, isect.dg.p,
					maxDistSquared);
				Lr += (k / nPaths) * photons[i].photon->alpha;
			}
			else {
				float k = kernel(photons[i].photon, isect.dg.p,
					maxDistSquared);
				Lt += (k / nPaths) * photons[i].photon->alpha;
			}
		}
		L += Lr * bsdf->rho(wo, BSDF_ALL_REFLECTION) * INV_PI +
			Lt * bsdf->rho(wo, BSDF_ALL_TRANSMISSION) * INV_PI;
	}
	return L;
}

void VolumePhotonMap::Surface_Init(int ncaus, int nind,
		int nl,	int mdepth, float mdist, bool fg,
		int gs, float rrt, float ga) {
	nCausticPhotons = ncaus;
	nIndirectPhotons = nind;
	nLookup = nl;
	maxDistSquared = mdist * mdist;
	maxSpecularDepth = mdepth;
	causticMap = indirectMap = volumeMap = NULL;
	radianceMap = NULL;
	specularDepth = 0;
	finalGather = fg;
	gatherSamples = gs;
	rrTreshold = rrt;
	cosGatherAngle = cos(Radians(ga));
	alreadyPreprocessed = false;
}

void VolumePhotonMap::Surface_Dispose() {
	delete causticMap;
	delete indirectMap;
	delete radianceMap;
	delete volumeMap;
}

void VolumePhotonMap::Surface_RequestSamples(Sample *sample,
		const Scene *scene) {
	// Allocate and request samples for sampling all lights
	u_int nLights = scene->lights.size();
	lightSampleOffset = new int[nLights];
	bsdfSampleOffset = new int[nLights];
	bsdfComponentOffset = new int[nLights];
	for (u_int i = 0; i < nLights; ++i) {
		const Light *light = scene->lights[i];
		int lightSamples =
			scene->sampler->RoundSize(light->nSamples);
		lightSampleOffset[i] = sample->Add2D(lightSamples);
		bsdfSampleOffset[i] = sample->Add2D(lightSamples);
		bsdfComponentOffset[i] = sample->Add1D(lightSamples);
	}
	lightNumOffset = -1;
	// Request samples for final gathering
	if (finalGather) {
		gatherSamples = scene->sampler->RoundSize(max(1,
			gatherSamples/2));
		gatherSampleOffset[0] = sample->Add2D(gatherSamples);
		gatherSampleOffset[1] = sample->Add2D(gatherSamples);
		gatherComponentOffset[0] = sample->Add1D(gatherSamples);
		gatherComponentOffset[1] = sample->Add1D(gatherSamples);
	}
}

//Uses Transmittance to determine ray termination. Also stops before hitting objects in the scene.
//Updates alpha and the albedo for ray absorption. Adapted from RS
bool VolumePhotonMap::RayMarch(const Scene * scene, Ray & marchRay, Point & loc, float tIntersect, Spectrum & alpha, float & albedo) {
	float t0 = marchRay.mint, t1 = min(marchRay.maxt, tIntersect);
	Point p = marchRay(t0), pPrev;
	t0 += RandomFloat() * stepSize;

	VolumeRegion * vr = scene->volumeRegion;

	Spectrum sigma_s(0.f);
	Spectrum sigma_t(0.f);

	Spectrum transmittance(1.f);

	Spectrum oldTransmittance(1.f);

	for(; t0 < t1; t0 += stepSize) {
		pPrev = p;
		p = marchRay(t0);
		loc = p;
		
		Spectrum stepTau = stepSize * vr->sigma_t(p, marchRay.d);		
		Spectrum tTransmittance = transmittance * Exp(-stepTau);
		
		if (RandomFloat() < 1 - tTransmittance.y()) {
			float scale = -logf(RandomFloat());
			t0 += (scale-1) * stepSize;
			p = marchRay(t0);
			loc = p;
			stepTau = scale * stepSize * vr->sigma_t(p, marchRay.d);
			transmittance = oldTransmittance * Exp(-stepTau);

			break;
		}

		oldTransmittance = transmittance;
		transmittance = tTransmittance;
	}
	
	alpha *= transmittance; 
	albedo = vr->sigma_s(loc, marchRay.d).y() / vr->sigma_t(loc, marchRay.d).y();
	return (t0 < t1);
	
}

//Traces rays through volume until termination at a surface or until leaves the surface
//Scattering is selected as the interaction with albedo probability. Source: RS.
//If rays scatter, they are scattered isotroppically (the phase function is isotropic, so equal
//probability in all directions). Therefore we modify by 1/4PI.
bool VolumePhotonMap::TraceThrough(const Scene * scene, RayDifferential & ray, Intersection & isect, Spectrum & alpha, 
	     int shot, bool & volumeDone, vector<Photon> & volumePhotons, ProgressReporter & progress) {
	
	// If no volume present
	if (scene->volumeRegion == NULL) return true;
	VolumeRegion * vr = scene->volumeRegion;

	Ray marchRay(ray.o, ray.d);
	vr->IntersectP(marchRay, &ray.mint, &ray.maxt);
	
	// The other surface is in front
	if (marchRay.mint > ray.maxt) return true;

	float albedo;

	Point loc;
	while(vr->IntersectP(marchRay, &marchRay.mint, &marchRay.maxt)
	      && RayMarch(scene, marchRay, loc, ray.maxt, alpha, albedo)) {
	
		
		if (!volumeDone) {
			volumePhotons.push_back(Photon(loc, alpha, -marchRay.d));
			progress.Update();
			if (nVolumePhotons == volumePhotons.size()) {
				volumeDone = true;
				nVolumePaths = shot;
				volumeMap = new KdTree<Photon, PhotonProcess>(volumePhotons);
			}
		}

		if (albedo < RandomFloat()) {
			return false;
		}
		alpha *= INV_TWOPI / 2; //alpha /= albedo;
		marchRay = Ray(loc, Normalize(UniformSampleSphere(RandomFloat(), RandomFloat())));
		ray = RayDifferential(marchRay);
		scene->Intersect(ray, &isect);
		//if (marchRay.mint > ray.maxt) return true;
	}

	ray = RayDifferential(marchRay);
	return scene->Intersect(ray, &isect); 
}

void VolumePhotonMap::Surface_Preprocess(const Scene *scene) {
	if (alreadyPreprocessed) return;
	alreadyPreprocessed = true;

	if (scene->lights.size() == 0) return;
	ProgressReporter progress(nCausticPhotons+ // NOBOOK
		nIndirectPhotons + nVolumePhotons, "Shooting photons"); // NOBOOK
	vector<Photon> causticPhotons;
	vector<Photon> indirectPhotons;
	vector<Photon> directPhotons;
	vector<Photon> volumePhotons;
	vector<RadiancePhoton> radiancePhotons;
	causticPhotons.reserve(nCausticPhotons); // NOBOOK
	indirectPhotons.reserve(nIndirectPhotons); // NOBOOK
	volumePhotons.reserve(nVolumePhotons);
	// Initialize photon shooting statistics
	static StatsCounter nshot("Photon Map",
		"Number of photons shot from lights");
	bool causticDone = (nCausticPhotons == 0);
	bool indirectDone = (nIndirectPhotons == 0);
	bool volumeDone = (nVolumePhotons == 0);
	// Compute light power CDF for photon shooting
	int nLights = int(scene->lights.size());
	float *lightPower = (float *)alloca(nLights * sizeof(float));
	float *lightCDF = (float *)alloca((nLights+1) * sizeof(float));
	for (int i = 0; i < nLights; ++i)
		lightPower[i] = scene->lights[i]->Power(scene).y();
	float totalPower;
	ComputeStep1dCDF(lightPower, nLights, &totalPower, lightCDF);
	// Declare radiance photon reflectance arrays
	vector<Spectrum> rpReflectances, rpTransmittances;

	while (!causticDone || !indirectDone || !volumeDone) {
		++nshot;
		// Give up if we're not storing enough photons
		if (nshot > 500000 &&
			(unsuccessful(nCausticPhotons,
			              causticPhotons.size(),
						  nshot) ||
			 unsuccessful(nIndirectPhotons,
			              indirectPhotons.size(),
						  nshot) ||
			 unsuccessful(nVolumePhotons,
				      volumePhotons.size(),
				      nshot))) {
			//Error("Unable to store enough photons.  Giving up.\n");
			Error("\nUnable to store enough photons.  Giving up.\n,"
				"  caustic  :%d\t  indirect :%d\t  volume   :%d\n",
				causticPhotons.size(), indirectPhotons.size(), volumePhotons.size());
			return;
		}
		// Trace a photon path and store contribution
		// Choose 4D sample values for photon
		float u[4];
		u[0] = RadicalInverse((int)nshot+1, 2);
		u[1] = RadicalInverse((int)nshot+1, 3);
		u[2] = RadicalInverse((int)nshot+1, 5);
		u[3] = RadicalInverse((int)nshot+1, 7);

		// Choose light to shoot photon from
		float lightPdf;
		float uln = RadicalInverse((int)nshot+1, 11);
		int lightNum = Floor2Int(SampleStep1d(lightPower, lightCDF,
				totalPower, nLights, uln, &lightPdf) * nLights);
		lightNum = min(lightNum, nLights-1);
		const Light *light = scene->lights[lightNum];
		// Generate _photonRay_ from light source and initialize _alpha_
		RayDifferential photonRay;
		float pdf;
		Spectrum alpha = light->Sample_L(scene, u[0], u[1], u[2], u[3],
			&photonRay, &pdf);
		if (pdf == 0.f || alpha.Black()) continue;
		alpha /= pdf * lightPdf;

		if (!alpha.Black()) {
			// Follow photon path through scene and record intersections
			bool specularPath = false;
			Intersection photonIsect;
			int nIntersections = 0;
			while (scene->Intersect(photonRay, &photonIsect)) {
				++nIntersections;
				
				// Handle photon/volume interaction
				if (!TraceThrough(scene, photonRay, photonIsect, alpha, nshot, volumeDone, 
						  volumePhotons, progress)) break;
				
				// Handle photon/surface intersection
				alpha *= scene->Transmittance(photonRay);
				Vector wo = -photonRay.d;
				BSDF *photonBSDF = photonIsect.GetBSDF(photonRay);
				BxDFType specularType = BxDFType(BSDF_REFLECTION |
					BSDF_TRANSMISSION | BSDF_SPECULAR);
				bool hasNonSpecular = (photonBSDF->NumComponents() >
					photonBSDF->NumComponents(specularType));
				if (hasNonSpecular) {
					// Deposit photon at surface
					Photon photon(photonIsect.dg.p, alpha, wo);
					if (nIntersections == 1) {
						// Deposit direct photon
						directPhotons.push_back(photon);
					}
					else {
						// Deposit either caustic or indirect photon
						if (specularPath) {
							// Process caustic photon intersection
							if (!causticDone) {
								causticPhotons.push_back(photon);
								if (causticPhotons.size() == nCausticPhotons) {
									causticDone = true;
									nCausticPaths = (int)nshot;
									causticMap = new KdTree<Photon, PhotonProcess>(causticPhotons);
								}
								progress.Update();
							}
						}
						else {
							// Process indirect lighting photon intersection
							if (!indirectDone) {
								indirectPhotons.push_back(photon);
								if (indirectPhotons.size() == nIndirectPhotons) {
									indirectDone = true;
									nIndirectPaths = (int)nshot;
									indirectMap = new KdTree<Photon, PhotonProcess>(indirectPhotons);
								}
								progress.Update();
							}
						}
					}
					if (finalGather && RandomFloat() < .125f) {
						// Store data for radiance photon
						static StatsCounter rp("Photon Map", "Radiance photons created"); // NOBOOK
						++rp; // NOBOOK
						Normal n = photonIsect.dg.nn;
						if (Dot(n, photonRay.d) > 0.f) n = -n;
						radiancePhotons.push_back(RadiancePhoton(photonIsect.dg.p, n));
						Spectrum rho_r = photonBSDF->rho(BSDF_ALL_REFLECTION);
						rpReflectances.push_back(rho_r);
						Spectrum rho_t = photonBSDF->rho(BSDF_ALL_TRANSMISSION);
						rpTransmittances.push_back(rho_t);
					}
				}
				// Sample new photon ray direction
				Vector wi;
				float pdf;
				BxDFType flags;
				// Get random numbers for sampling outgoing photon direction
				float u1, u2, u3;
				if (nIntersections == 1) {
					u1 = RadicalInverse((int)nshot+1, 13);
					u2 = RadicalInverse((int)nshot+1, 17);
					u3 = RadicalInverse((int)nshot+1, 19);
				}
				else {
					u1 = RandomFloat();
					u2 = RandomFloat();
					u3 = RandomFloat();
				}

				// Compute new photon weight and possibly terminate with RR
				Spectrum fr = photonBSDF->Sample_f(wo, &wi, u1, u2, u3,
				                                   &pdf, BSDF_ALL, &flags);
				if (fr.Black() || pdf == 0.f)
					break;
				Spectrum anew = alpha * fr *
					AbsDot(wi, photonBSDF->dgShading.nn) / pdf;
				float continueProb = min(1.f, anew.y() / alpha.y());
				if (RandomFloat() > continueProb || nIntersections > 10)
					break;
				alpha = anew / continueProb;
				specularPath = (nIntersections == 1 || specularPath) &&
					((flags & BSDF_SPECULAR) != 0);
				photonRay = RayDifferential(photonIsect.dg.p, wi);
			}
		}
		BSDF::FreeAll();
	}

	progress.Done(); // NOBOOK

	// Precompute radiance at a subset of the photons
	KdTree<Photon, PhotonProcess> directMap(directPhotons);
	int nDirectPaths = nshot;
	if (finalGather) {
		ProgressReporter p2(radiancePhotons.size(), "Computing photon radiances"); // NOBOOK
		for (u_int i = 0; i < radiancePhotons.size(); ++i) {
			// Compute radiance for radiance photon _i_
			RadiancePhoton &rp = radiancePhotons[i];
			const Spectrum &rho_r = rpReflectances[i];
			const Spectrum &rho_t = rpTransmittances[i];
			Spectrum E;
			Point p = rp.p;
			Normal n = rp.n;
			if (!rho_r.Black()) {
				E = Surface_estimateE(&directMap,  nDirectPaths,   p, n) +
					Surface_estimateE(indirectMap, nIndirectPaths, p, n) +
					Surface_estimateE(causticMap,  nCausticPaths,  p, n);
				rp.Lo += E * INV_PI * rho_r;
			}
			if (!rho_t.Black()) {
				E = Surface_estimateE(&directMap,  nDirectPaths,   p, -n) +
					Surface_estimateE(indirectMap, nIndirectPaths, p, -n) +
					Surface_estimateE(causticMap,  nCausticPaths,  p, -n);
				rp.Lo += E * INV_PI * rho_t;
			}
			p2.Update(); // NOBOOK
		}
		radianceMap = new KdTree<RadiancePhoton,
			RadiancePhotonProcess>(radiancePhotons);
		p2.Done(); // NOBOOK
	}
}

Spectrum VolumePhotonMap::Surface_Li(const Scene *scene,
		const RayDifferential &ray, const Sample *sample,
		float *alpha) const {
	// Compute reflected radiance with photon map
	Spectrum L(0.);
	Intersection isect;
	if (scene->Intersect(ray, &isect)) {
		if (alpha) *alpha = 1.;
		Vector wo = -ray.d;
		// Compute emitted light if ray hit an area light source
		L += isect.Le(wo);
		// Evaluate BSDF at hit point
		BSDF *bsdf = isect.GetBSDF(ray);
		const Point &p = bsdf->dgShading.p;
		const Normal &n = bsdf->dgShading.nn;
		L += UniformSampleAllLights(scene, p, n,
			wo, bsdf, sample,
			lightSampleOffset, bsdfSampleOffset,
			bsdfComponentOffset);

		// Compute indirect lighting for photon map integrator
		L += Surface_LPhoton(causticMap, nCausticPaths, nLookup, bsdf,
			isect, wo, maxDistSquared);
		if (finalGather) {
#if 1
			// Do one-bounce final gather for photon map
			BxDFType nonSpecular = BxDFType(BSDF_REFLECTION |
				BSDF_TRANSMISSION | BSDF_DIFFUSE | BSDF_GLOSSY);
			if (bsdf->NumComponents(nonSpecular) > 0) {
				// Find indirect photons around point for importance sampling
				u_int nIndirSamplePhotons = 50;
				PhotonProcess proc(nIndirSamplePhotons, p);
				proc.photons = (ClosePhoton *)alloca(nIndirSamplePhotons *
					sizeof(ClosePhoton));
				float searchDist2 = maxDistSquared;
				while (proc.foundPhotons < nIndirSamplePhotons) {
					float md2 = searchDist2;
					proc.foundPhotons = 0;
					indirectMap->Lookup(p, proc, md2);
					searchDist2 *= 2.f;
				}
				// Copy photon directions to local array
				Vector *photonDirs = (Vector *)alloca(nIndirSamplePhotons *
					sizeof(Vector));
				for (u_int i = 0; i < nIndirSamplePhotons; ++i)
					photonDirs[i] = proc.photons[i].photon->wi;
				// Use BSDF to do final gathering
				Spectrum Li = 0.;
				static StatsCounter gatherRays("Photon Map", // NOBOOK
					"Final gather rays traced"); // NOBOOK
				for (int i = 0; i < gatherSamples; ++i) {
					// Sample random direction from BSDF for final gather ray
					Vector wi;
					float u1 = sample->twoD[gatherSampleOffset[0]][2*i];
					float u2 = sample->twoD[gatherSampleOffset[0]][2*i+1];
					float u3 = sample->oneD[gatherComponentOffset[0]][i];
					float pdf;
					Spectrum fr = bsdf->Sample_f(wo, &wi, u1, u2, u3,
						&pdf, BxDFType(BSDF_ALL & (~BSDF_SPECULAR)));
					if (fr.Black() || pdf == 0.f) continue;
					// Trace BSDF final gather ray and accumulate radiance
					RayDifferential bounceRay(p, wi);
					++gatherRays; // NOBOOK
					Intersection gatherIsect;
					if (scene->Intersect(bounceRay, &gatherIsect)) {
						// Compute exitant radiance using precomputed irradiance
						Spectrum Lindir = 0.f;
						Normal n = gatherIsect.dg.nn;
						if (Dot(n, bounceRay.d) > 0) n = -n;
						RadiancePhotonProcess proc(gatherIsect.dg.p, n);
						float md2 = INFINITY;
						radianceMap->Lookup(gatherIsect.dg.p, proc, md2);
						if (proc.photon)
							Lindir = proc.photon->Lo;
						Lindir *= scene->Transmittance(bounceRay);
						// Compute MIS weight for BSDF-sampled gather ray
						// Compute PDF for photon-sampling of direction _wi_
						float photonPdf = 0.f;
						float conePdf = UniformConePdf(cosGatherAngle);
						for (u_int j = 0; j < nIndirSamplePhotons; ++j)
							if (Dot(photonDirs[j], wi) > .999f * cosGatherAngle)
								photonPdf += conePdf;
						photonPdf /= nIndirSamplePhotons;
						float wt = PowerHeuristic(gatherSamples, pdf,
							gatherSamples, photonPdf);
						Li += fr * Lindir * AbsDot(wi, n) * wt / pdf;
					}
				}
				L += Li / gatherSamples;
				// Use nearby photons to do final gathering
				Li = 0.;
				for (int i = 0; i < gatherSamples; ++i) {
					// Sample random direction using photons for final gather ray
					float u1 = sample->oneD[gatherComponentOffset[1]][i];
					float u2 = sample->twoD[gatherSampleOffset[1]][2*i];
					float u3 = sample->twoD[gatherSampleOffset[1]][2*i+1];
					int photonNum = min((int)nIndirSamplePhotons - 1,
						Floor2Int(u1 * nIndirSamplePhotons));
					// Sample gather ray direction from _photonNum_
					Vector vx, vy;
					CoordinateSystem(photonDirs[photonNum], &vx, &vy);
					Vector wi = UniformSampleCone(u2, u3, cosGatherAngle, vx, vy,
						photonDirs[photonNum]);
					// Trace photon-sampled final gather ray and accumulate radiance
					Spectrum fr = bsdf->f(wo, wi);
					if (fr.Black()) continue;
					// Compute PDF for photon-sampling of direction _wi_
					float photonPdf = 0.f;
					float conePdf = UniformConePdf(cosGatherAngle);
					for (u_int j = 0; j < nIndirSamplePhotons; ++j)
						if (Dot(photonDirs[j], wi) > .999f * cosGatherAngle)
							photonPdf += conePdf;
					photonPdf /= nIndirSamplePhotons;
					RayDifferential bounceRay(p, wi);
					++gatherRays; // NOBOOK
					Intersection gatherIsect;
					if (scene->Intersect(bounceRay, &gatherIsect)) {
						// Compute exitant radiance using precomputed irradiance
						Spectrum Lindir = 0.f;
						Normal n = gatherIsect.dg.nn;
						if (Dot(n, bounceRay.d) > 0) n = -n;
						RadiancePhotonProcess proc(gatherIsect.dg.p, n);
						float md2 = INFINITY;
						radianceMap->Lookup(gatherIsect.dg.p, proc, md2);
						if (proc.photon)
							Lindir = proc.photon->Lo;
						Lindir *= scene->Transmittance(bounceRay);
						// Compute MIS weight for photon-sampled gather ray
						float bsdfPdf = bsdf->Pdf(wo, wi);
						float wt = PowerHeuristic(gatherSamples, photonPdf,
								gatherSamples, bsdfPdf);
						Li += fr * Lindir * AbsDot(wi, n) * wt / photonPdf;
					}
				}
				L += Li / gatherSamples;
			}
#else
// look at radiance map directly..
Normal nn = n;
if (Dot(nn, ray.d) > 0.) nn = -n;
RadiancePhotonProcess proc(p, nn);
float md2 = INFINITY;
radianceMap->Lookup(p, proc, md2);
if (proc.photon)
	L += proc.photon->Lo;
#endif

		}
		else {
		    L += Surface_LPhoton(indirectMap, nIndirectPaths, nLookup,
				 bsdf, isect, wo, maxDistSquared);
		}
		if (specularDepth++ < maxSpecularDepth) {
			Vector wi;
			// Trace rays for specular reflection and refraction
			Spectrum f = bsdf->Sample_f(wo, &wi,
				BxDFType(BSDF_REFLECTION | BSDF_SPECULAR));
			if (!f.Black()) {
				// Compute ray differential _rd_ for specular reflection
				RayDifferential rd(p, wi);
				rd.hasDifferentials = true;
				rd.rx.o = p + isect.dg.dpdx;
				rd.ry.o = p + isect.dg.dpdy;
				// Compute differential reflected directions
				Normal dndx = bsdf->dgShading.dndu * bsdf->dgShading.dudx +
					bsdf->dgShading.dndv * bsdf->dgShading.dvdx;
				Normal dndy = bsdf->dgShading.dndu * bsdf->dgShading.dudy +
					bsdf->dgShading.dndv * bsdf->dgShading.dvdy;
				Vector dwodx = -ray.rx.d - wo, dwody = -ray.ry.d - wo;
				float dDNdx = Dot(dwodx, n) + Dot(wo, dndx);
				float dDNdy = Dot(dwody, n) + Dot(wo, dndy);
				rd.rx.d = wi -
				          dwodx + 2 * Vector(Dot(wo, n) * dndx +
						  dDNdx * n);
				rd.ry.d = wi -
				          dwody + 2 * Vector(Dot(wo, n) * dndy +
						  dDNdy * n);
				L += scene->Li(rd, sample) * f * AbsDot(wi, n);
			}
			f = bsdf->Sample_f(wo, &wi,
				BxDFType(BSDF_TRANSMISSION | BSDF_SPECULAR));
			if (!f.Black()) {
				// Compute ray differential _rd_ for specular transmission
				RayDifferential rd(p, wi);
				rd.hasDifferentials = true;
				rd.rx.o = p + isect.dg.dpdx;
				rd.ry.o = p + isect.dg.dpdy;

				float eta = bsdf->eta;
				Vector w = -wo;
				if (Dot(wo, n) < 0) eta = 1.f / eta;

				Normal dndx = bsdf->dgShading.dndu * bsdf->dgShading.dudx + bsdf->dgShading.dndv * bsdf->dgShading.dvdx;
				Normal dndy = bsdf->dgShading.dndu * bsdf->dgShading.dudy + bsdf->dgShading.dndv * bsdf->dgShading.dvdy;

				Vector dwodx = -ray.rx.d - wo, dwody = -ray.ry.d - wo;
				float dDNdx = Dot(dwodx, n) + Dot(wo, dndx);
				float dDNdy = Dot(dwody, n) + Dot(wo, dndy);

				float mu = eta * Dot(w, n) - Dot(wi, n);
				float dmudx = (eta - (eta*eta*Dot(w,n))/Dot(wi, n)) * dDNdx;
				float dmudy = (eta - (eta*eta*Dot(w,n))/Dot(wi, n)) * dDNdy;

				rd.rx.d = wi + eta * dwodx - Vector(mu * dndx + dmudx * n);
				rd.ry.d = wi + eta * dwody - Vector(mu * dndy + dmudy * n);
				L += scene->Li(rd, sample) * f * AbsDot(wi, n);
			}
		}
		--specularDepth;
	}
	else {
		// Handle ray with no intersection
		if (alpha) *alpha = 0.;
		for (u_int i = 0; i < scene->lights.size(); ++i)
			L += scene->lights[i]->Le(ray);
		if (alpha && !L.Black()) *alpha = 1.;
		return L;
	}
	return L;
}

// SingleScattering Method Definitions
void VolumePhotonMap::Volume_RequestSamples(Sample *sample,
		const Scene *scene) {
	tauSampleOffset = sample->Add1D(1);
	scatterSampleOffset = sample->Add1D(1);
}
Spectrum VolumePhotonMap::Volume_Transmittance(const Scene *scene,
		const Ray &ray, const Sample *sample, float *alpha) const {
	if (!scene->volumeRegion) return Spectrum(1.f);
	float step = sample ? stepSize : 4.f * stepSize;
	float offset = sample ? sample->oneD[tauSampleOffset][0] :
		RandomFloat();
	Spectrum tau = scene->volumeRegion->Tau(ray, step, offset);
	return Exp(-tau);
}

Spectrum VolumePhotonMap::Volume_Li(const Scene *scene,
		const RayDifferential &ray, const Sample *sample,
		float *alpha) const {
	VolumeRegion *vr = scene->volumeRegion;
	float t0, t1;
	if (!vr || !vr->IntersectP(ray, &t0, &t1)) return 0.f;
	// Do single scattering volume integration in _vr_
	Spectrum Lv(0.);
	// Prepare for volume integration stepping
	int N = Ceil2Int((t1-t0) / stepSize);
	float step = (t1 - t0) / N;
	Spectrum Tr(1.f);
	Point p = ray(t0), pPrev;
	Vector w = -ray.d;
	if (sample)
		t0 += sample->oneD[scatterSampleOffset][0] * step;
	else
		t0 += RandomFloat() * step;
	// Compute sample patterns for single scattering samples
	/*	float *samp = (float *)alloca(3 * N * sizeof(float));
	LatinHypercube(samp, N, 3);
	int sampOffset = 0;
	*/
	float md2 = maxDistSquared;
	float scale = 0.75f / (M_PI * float(nVolumePaths));
	
	for (int i = 0; i < N; ++i, t0 += step) {
		// Advance to sample at _t0_ and update _T_
		pPrev = p;
		p = ray(t0);
		Spectrum stepTau = vr->Tau(Ray(pPrev, p - pPrev, 0, 1),
			.5f * stepSize, RandomFloat());
		Tr *= Exp(-stepTau);
		// Possibly terminate raymarching if transmittance is small
		if (Tr.y() < 1e-3) {
			const float continueProb = .5f;
			if (RandomFloat() > continueProb) break;
			Tr /= continueProb;
		}
		// Compute single-scattering source term at _p_
		Lv += Tr * vr->Lve(p, w);

		/*		Spectrum ss = vr->sigma_s(p, w);
		if (!ss.Black() && scene->lights.size() > 0) {
			int nLights = scene->lights.size();
			int lightNum =
				min(Floor2Int(samp[sampOffset] * nLights),
				    nLights-1);
			Light *light = scene->lights[lightNum];
			// Add contribution of _light_ due to scattering at _p_
			float pdf;
			VisibilityTester vis;
			Vector wo;
			float u1 = samp[sampOffset+1], u2 = samp[sampOffset+2];
			Spectrum L = light->Sample_L(p, u1, u2, &wo, &pdf, &vis);
			if (!L.Black() && pdf > 0.f && vis.Unoccluded(scene)) {
				Spectrum Ld = L * vis.Transmittance(scene);
				Lv += Tr * ss * vr->p(p, w, -wo) *
				Ld * float(nLights) / pdf;
			}
		}
		sampOffset += 3;
		*/
		// Compute multi-scattering source term at _p_
		// [Source: Suwandi and Xu]
		if (volumeMap != NULL) {
			PhotonProcess proc(nLookup, p);
			proc.photons = (ClosePhoton *)alloca(nLookup * sizeof(ClosePhoton));
			md2 = maxDistSquared;
			volumeMap->Lookup(p, proc, md2);
			if (md2 > 0.0f) {
				Spectrum Lm = Spectrum(0.0f);
				for (u_int j = 0; j < proc.foundPhotons; ++j) {
					const Photon& photon = *proc.photons[j].photon;
					Lm += vr->p(p, photon.wi, w) * photon.alpha;
				}
				Lv += Tr * Lm * scale / powf(md2, 1.5f);
			}
		}
	}
	return Lv * step;
}

VolumePhotonMap globalPhotonMap;

class SurfaceIntegratorWrapper : public SurfaceIntegrator {
public:
	SurfaceIntegratorWrapper(int ncaus, int nindir, int nLookup, int mdepth,
			 float maxdist, bool finalGather, int gatherSamples,
			 float rrt, float ga) {
		globalPhotonMap.Surface_Init(ncaus, nindir, nLookup, mdepth,
			 maxdist, finalGather, gatherSamples,
			 rrt, ga);
	}

	~SurfaceIntegratorWrapper() { globalPhotonMap.Surface_Dispose(); }
	
	Spectrum Li(const Scene *scene, const RayDifferential &ray,
		    const Sample *sample, float *alpha) const {
		return globalPhotonMap.Surface_Li(scene, ray, sample, alpha);
	}

	void Preprocess(const Scene *scene) {
		globalPhotonMap.Surface_Preprocess(scene);
	}
	
	void RequestSamples(Sample *sample,
	                            const Scene *scene) {
		globalPhotonMap.Surface_RequestSamples(sample, scene);
	}
};

class VolumeIntegratorWrapper : public VolumeIntegrator {
public:
	VolumeIntegratorWrapper(int nVolume, float ss) {
		globalPhotonMap.Volume_Init(nVolume, ss);
	}

	~VolumeIntegratorWrapper() { globalPhotonMap.Volume_Dispose(); }

	Spectrum Li(const Scene *scene, const RayDifferential &ray,
		    const Sample *sample, float *alpha) const {
		return globalPhotonMap.Volume_Li(scene, ray, sample, alpha);
	}

	void Preprocess(const Scene *scene) {
		globalPhotonMap.Surface_Preprocess(scene);
	}

	void RequestSamples(Sample *sample, const Scene *scene) {
		globalPhotonMap.Volume_RequestSamples(sample, scene);
	}

	Spectrum Transmittance(const Scene *scene, const Ray &ray, const Sample *sample,
			       float *alpha) const {
		return globalPhotonMap.Volume_Transmittance(scene, ray, sample, alpha);
	}
};

extern "C" DLLEXPORT SurfaceIntegrator *CreateSurfaceIntegrator(const ParamSet &params) {
	int nCaustic = params.FindOneInt("causticphotons", 20000);
	int nIndirect = params.FindOneInt("indirectphotons", 100000);
	int nUsed = params.FindOneInt("nused", 50);
	int maxDepth = params.FindOneInt("maxdepth", 5);
	bool finalGather = params.FindOneBool("finalgather", true);
	int gatherSamples = params.FindOneInt("finalgathersamples", 32);
	float maxDist = params.FindOneFloat("maxdist", .1f);
	float rrTreshold = params.FindOneFloat("rrthreshold", .05f);
	float gatherAngle = params.FindOneFloat("gatherangle", 10.f);
	return new SurfaceIntegratorWrapper(nCaustic, nIndirect,
		nUsed, maxDepth, maxDist, finalGather, gatherSamples,
		rrTreshold, gatherAngle);
}

extern "C" DLLEXPORT VolumeIntegrator *CreateVolumeIntegrator(const ParamSet &params) {
	int nVolume = params.FindOneInt("volumephotons", 50000);
	float stepSize  = params.FindOneFloat("stepsize", 1.f);
	return new VolumeIntegratorWrapper(nVolume, stepSize);
}