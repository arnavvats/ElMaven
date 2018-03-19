#include "IsotopeDetection.h"

IsotopeDetection::IsotopeDetection(
    MavenParameters *mavenParameters,
    IsotopeDetectionType isoType,
    bool C13Flag,
    bool N15Flag,
    bool S34Flag,
    bool D2Flag)
{
    _mavenParameters = mavenParameters;
    _isoType = isoType;
    _C13Flag = C13Flag;
    _N15Flag = N15Flag;
    _S34Flag = S34Flag;
    _D2Flag = D2Flag;

}

void IsotopeDetection::pullIsotopes(PeakGroup* parentgroup)
{
    // FALSE CONDITIONS
    if (parentgroup == NULL)
        return;
    if (parentgroup->compound == NULL)
        return;
    if (parentgroup->compound->formula.empty() == true)
        return;
    if (_mavenParameters->samples.size() == 0)
        return;

    string formula = parentgroup->compound->formula; //parent formula
    int charge = _mavenParameters->getCharge(parentgroup->compound);//generate isotope list for parent mass

    vector<Isotope> masslist = MassCalculator::computeIsotopes(
        formula,
        charge,
        _C13Flag,
        _N15Flag,
        _S34Flag,
        _D2Flag
    );

    map<string, PeakGroup> isotopes = getIsotopes(parentgroup, masslist);

    addIsotopes(parentgroup, isotopes);

}

map<string, PeakGroup> IsotopeDetection::getIsotopes(PeakGroup* parentgroup, vector<Isotope> masslist)
{
    //iterate over samples to find properties for parent's isotopes.
    map<string, PeakGroup> isotopes;

    for (unsigned int s = 0; s < _mavenParameters->samples.size(); s++) {
        mzSample* sample = _mavenParameters->samples[s];
        for (unsigned int k = 0; k < masslist.size(); k++) {
            // if (stopped())
            //     break; TODO: stop
    
            Isotope& x = masslist[k];

            float mzmin = x.mass -_mavenParameters->compoundMassCutoffWindow->massCutoffValue(x.mass);
            float mzmax = x.mass +_mavenParameters->compoundMassCutoffWindow->massCutoffValue(x.mass);
            float rtmin = sample->minRt;
            float rtmax = sample->maxRt;
            mzSlice* slice = new mzSlice(mzmin, mzmax, rtmin, rtmax);

            slice->rt = parentgroup->medianRt();
            Peak* parentPeak = parentgroup->getPeak(sample);
            if (parentPeak)
                slice->rt = parentPeak->rt;

            float isotopePeakIntensity = 0;
            float parentPeakIntensity = 0;

            if (parentPeak) {
                parentPeakIntensity = parentPeak->peakIntensity;
                Scan* scan = parentPeak->getScan();
                std::pair<float, float> isotope = getIntensity(scan, mzmin, mzmax);
                isotopePeakIntensity = isotope.first;
                slice->rt = isotope.second;
            }

            if (filterIsotope(x, isotopePeakIntensity, parentPeakIntensity, sample, parentgroup))
                continue;
            
            vector<Peak> allPeaks;


            vector<mzSample*> samples;
            samples.push_back(sample);

            //TODO: pullEICs should be able to have mzSample as an argument
            EIC *eic = PeakDetector::pullEICs(
                slice,
                samples,
                _mavenParameters->eic_smoothingWindow,
                _mavenParameters->eic_smoothingAlgorithm, 
                _mavenParameters->amuQ1,
                _mavenParameters->amuQ3,
                _mavenParameters->baseline_smoothingWindow,
                _mavenParameters->baseline_dropTopX, 
                _mavenParameters->isotopicMinSignalBaselineDifference,
                _mavenParameters->eicType,
                _mavenParameters->filterline)[0]; 

            //TODO: this needs be optimized to not bother finding peaks outside of
            //maxIsotopeScanDiff window
            allPeaks = eic->peaks;

            bool isIsotope = true;
			PeakFiltering peakFiltering(_mavenParameters, isIsotope);
            peakFiltering.filter(allPeaks);

            delete(eic);
            // find nearest peak as long as it is within RT window
            float maxRtDiff=_mavenParameters->maxIsotopeScanDiff * _mavenParameters->avgScanTime;
            //why are we even doing this calculation, why not have the parameter be in units of RT?
            Peak* nearestPeak = NULL;
            float d = FLT_MAX;
            for (unsigned int i = 0; i < allPeaks.size(); i++) {
                Peak& x = allPeaks[i];
                float dist = abs(x.rt - slice->rt);
                if (dist > maxRtDiff)
                    continue;
                if (dist < d) {
                    d = dist;
                    nearestPeak = &x;
                }
            }

            //delete (nearestPeak);
            if (nearestPeak) { //if nearest peak is present
                if (isotopes.count(x.name) == 0) { //label the peak of isotope
                    PeakGroup g;
                    g.meanMz = x.mass; //This get's updated in groupStatistics function
                    g.expectedMz = x.mass;
                    g.tagString = x.name;
                    g.expectedAbundance = x.abundance;
                    g.isotopeC13count = x.C13;
                    g.setSelectedSamples(parentgroup->samples);
                    isotopes[x.name] = g;
                }
                isotopes[x.name].addPeak(*nearestPeak); //add nearestPeak to isotope peak list
            }
            vector<Peak>().swap(allPeaks);
        }
    }
    return isotopes;
}

bool IsotopeDetection::filterIsotope(Isotope x, float isotopePeakIntensity, float parentPeakIntensity, mzSample* sample, PeakGroup* parentGroup)
{

    //TODO: I think this loop will never run right? Since we're now only pulling the relevant isotopes
    //if x.C13>0 then _mavenParameters->C13Labeled_BPE must have been true
    //so we could just eliminate maxNaturalAbundanceErr parameter in this case
    //original idea (see https://github.com/ElucidataInc/ElMaven/issues/43) was to have different checkboxes for "use this element for natural abundance check"
    if ((x.C13 > 0 && _C13Flag == false)
        || (x.N15 > 0 && _N15Flag == false)
        || (x.S34 > 0 && _S34Flag == false)
        || (x.H2 > 0 && _D2Flag == false)
        )
    {
        float expectedAbundance = x.abundance;
        if (expectedAbundance < 1e-8)
            return true;

        /**
         * TODO: In practice this is probably fine but in general 
         * I don't like these types of intensity checks -- the actual
         * absolute value depends on the type of instrument, etc
         */
        if (expectedAbundance * parentPeakIntensity < 1)
            return true;

        float observedAbundance = isotopePeakIntensity
            / (parentPeakIntensity + isotopePeakIntensity); //find observedAbundance based on isotopePeakIntensity

        float naturalAbundanceError = abs(
                observedAbundance - expectedAbundance) //if observedAbundance is significant wrt expectedAbundance
            / expectedAbundance * 100; // compute natural Abundance Error

        if (naturalAbundanceError >
                _mavenParameters->maxNaturalAbundanceErr)
            return true;
    }

    //TODO: this is really an abuse of the maxIsotopeScanDiff parameter
    //I can easily imagine you might set maxIsotopeScanDiff to something much less than the peak width
    //here w should really be determined by the minRt and maxRt for the parent and child peaks
    if (parentGroup)
    {
        Peak* parentPeak = parentGroup->getPeak(sample);
        float rtmin = parentGroup->minRt;
        float rtmax = parentGroup->maxRt;
        if (parentPeak)
        {
            rtmin = parentPeak->rtmin;
            rtmax = parentPeak->rtmax;
        }
        float isotopeMass = x.mass;
        float parentMass = parentGroup->meanMz;
        float w = _mavenParameters->maxIsotopeScanDiff
            * _mavenParameters->avgScanTime;
        double c = sample->correlation(
                isotopeMass, parentMass,
                _mavenParameters->compoundMassCutoffWindow, rtmin - w,
                rtmax + w, _mavenParameters->eicType,
            _mavenParameters->filterline);  // find correlation for isotopes
        if (c < _mavenParameters->minIsotopicCorrelation)
            return true;
    }
    return false;
}

std::pair<float, float> IsotopeDetection::getIntensity(Scan* scan, float mzmin, float mzmax)
{
    float highestIntensity = 0;
    float rt = 0;
    mzSample* sample = scan->getSample();
    //TODO: use maxIsotopeScanDiff instead of arbitrary number
    for (int i = scan->scannum - 2; i < scan->scannum + 2; i++) {
		Scan* s = sample->getScan(i);
		vector<int> matches = s->findMatchingMzs(mzmin, mzmax);
		for (auto pos:matches) {
			if (s->intensity[pos] > highestIntensity)
				highestIntensity = s->intensity[pos];
            rt = s->rt;
		}
	}
    return std::make_pair(highestIntensity, rt);
}

void IsotopeDetection::addIsotopes(PeakGroup* parentgroup, map<string, PeakGroup> isotopes)
{

    map<string, PeakGroup>::iterator itrIsotope;
    unsigned int index = 1;
    for (itrIsotope = isotopes.begin(); itrIsotope != isotopes.end(); ++itrIsotope, index++) {
        string isotopeName = (*itrIsotope).first;
        PeakGroup& child = (*itrIsotope).second;
        child.metaGroupId = index;

        childStatistics(parentgroup, child, isotopeName);
        bool isotopeAdded = filterLabel(isotopeName);
        if (!isotopeAdded) continue;
        
        addChild(parentgroup, child, isotopeName);
    }
}

void IsotopeDetection::addChild(PeakGroup *parentgroup, PeakGroup &child, string isotopeName)
{

    bool childExist;

    switch (_isoType)
    {
        case IsotopeDetectionType::PeakDetection:
            childExist = checkChildExist(parentgroup->children, isotopeName);
            if (!childExist) parentgroup->addChild(child);
            break;
        case IsotopeDetectionType::IsoWidget:
            childExist = checkChildExist(parentgroup->childrenIsoWidget, isotopeName);
            if (!childExist) parentgroup->addChildIsoWidget(child);
            break;
        case IsotopeDetectionType::BarPlot:
            childExist = checkChildExist(parentgroup->childrenBarPlot, isotopeName);
            if (!childExist) parentgroup->addChildBarPlot(child);
            break;
    }
}

bool IsotopeDetection::checkChildExist(vector<PeakGroup> &children, string isotopeName)
{

    bool childExist = false;
    for (unsigned int ii = 0; ii < children.size(); ii++) {
        if (children[ii].tagString == isotopeName) {
            childExist = true;
        }
    }

    return childExist;
}

void IsotopeDetection::childStatistics(
                        PeakGroup* parentgroup,
                        PeakGroup &child,
                        string isotopeName)
{

    child.tagString = isotopeName;
    child.groupId = parentgroup->groupId;
    child.compound = parentgroup->compound;
    child.parent = parentgroup;
    child.setType(PeakGroup::Isotope);
    child.groupStatistics();
    if (_mavenParameters->clsf->hasModel()) {
        _mavenParameters->clsf->classify(&child);
        child.groupStatistics();
    }

    bool deltaRtCheckFlag = _mavenParameters->deltaRtCheckFlag;
    float compoundRTWindow = _mavenParameters->compoundRTWindow;
    int qualityWeight = _mavenParameters->qualityWeight;
    int intensityWeight = _mavenParameters->intensityWeight;
    int deltaRTWeight = _mavenParameters->deltaRTWeight;

    child.calGroupRank(deltaRtCheckFlag,
                       compoundRTWindow,
                       qualityWeight,
                       intensityWeight,
                       deltaRTWeight);

}

bool IsotopeDetection::filterLabel(string isotopeName)
{

    if (!_C13Flag) {
        if (isotopeName.find(C13_LABEL) != string::npos)
            return false;
        else if (isotopeName.find(C13N15_LABEL) != string::npos)
            return false;
        else if (isotopeName.find(C13S34_LABEL) != string::npos)
            return false;
    }

    if (!_N15Flag) {
        if (isotopeName.find(N15_LABEL) != string::npos)
            return false;
        else if (isotopeName.find(C13N15_LABEL) != string::npos)
            return false;
    }

    if (!_S34Flag) {
        if (isotopeName.find(S34_LABEL) != string::npos)
            return false;
        else if (isotopeName.find(C13S34_LABEL) != string::npos)
            return false;
    }

    if (!_D2Flag) {
        if (isotopeName.find(H2_LABEL) != string::npos)
            return false;
    }

    return true;

}
