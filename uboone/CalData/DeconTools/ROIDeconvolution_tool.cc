////////////////////////////////////////////////////////////////////////
/// \file   ROIDeconvolution.cc
/// \author T. Usher
////////////////////////////////////////////////////////////////////////

#include <cmath>
#include "uboone/CalData/DeconTools/IDeconvolution.h"
#include "art/Utilities/ToolMacros.h"
#include "art/Framework/Services/Optional/TFileService.h"
#include "messagefacility/MessageLogger/MessageLogger.h"
#include "cetlib/exception.h"
#include "lardata/DetectorInfoServices/DetectorPropertiesService.h"
#include "larcore/Geometry/Geometry.h"
#include "uboone/Utilities/SignalShapingServiceMicroBooNE.h"
#include "lardata/Utilities/LArFFT.h"

#include "art/Utilities/make_tool.h"
#include "uboone/CalData/DeconTools/IBaseline.h"

#include "TH1D.h"

#include <fstream>

namespace uboone_tool
{

class ROIDeconvolution : IDeconvolution
{
public:
    explicit ROIDeconvolution(const fhicl::ParameterSet& pset);
    
    ~ROIDeconvolution();
    
    void configure(const fhicl::ParameterSet& pset)              override;
    void outputHistograms(art::TFileDirectory&)            const override;
    
    void Deconvolve(IROIFinder::Waveform&,
                    raw::ChannelID_t,
                    IROIFinder::CandidateROIVec&,
                    recob::Wire::RegionsOfInterest_t& )    const override;
    
private:
    // Member variables from the fhicl file
    size_t                                                   fFFTSize;                    ///< FFT size for ROI deconvolution
    float                                                    fMinROIAverageTickThreshold; ///< try to remove bad ROIs
    bool                                                     fDodQdxCalib;                ///< Do we apply wire-by-wire calibration?
    std::string                                              fdQdxCalibFileName;          ///< Text file for constants to do wire-by-wire calibration
    std::map<unsigned int, float>                            fdQdxCalib;                  ///< Map to do wire-by-wire calibration, key is channel
    ///< number, content is correction factor
    
    std::unique_ptr<uboone_tool::IBaseline>                  fBaseline;
    
    const geo::GeometryCore*                                 fGeometry = lar::providerFrom<geo::Geometry>();
    art::ServiceHandle<util::LArFFT>                         fFFT;
    art::ServiceHandle<util::SignalShapingServiceMicroBooNE> fSignalShaping;
};
    
//----------------------------------------------------------------------
// Constructor.
ROIDeconvolution::ROIDeconvolution(const fhicl::ParameterSet& pset)
{
    configure(pset);
}
    
ROIDeconvolution::~ROIDeconvolution()
{
}
    
void ROIDeconvolution::configure(const fhicl::ParameterSet& pset)
{
    // Start by recovering the parameters
    fFFTSize                    = pset.get< size_t >("FFTSize"                );
    fMinROIAverageTickThreshold = pset.get<float>   ("MinROIAverageTickThreshold",-0.5);
    
    //wire-by-wire calibration
    fDodQdxCalib = pset.get< bool >("DodQdxCalib", false);
    
    if (fDodQdxCalib)
    {
        fdQdxCalibFileName = pset.get< std::string >("dQdxCalibFileName");
        std::string fullname;
        cet::search_path sp("FW_SEARCH_PATH");
        sp.find_file(fdQdxCalibFileName, fullname);
        
        if (fullname.empty())
        {
            std::cout << "Input file " << fdQdxCalibFileName << " not found" << std::endl;
            throw cet::exception("File not found");
        }
        else
            std::cout << "Applying wire-by-wire calibration using file " << fdQdxCalibFileName << std::endl;
        
        std::ifstream inFile(fullname, std::ios::in);
        std::string line;
        
        while (std::getline(inFile,line))
        {
            unsigned int channel;
            float        constant;
            std::stringstream linestream(line);
            linestream >> channel >> constant;
            fdQdxCalib[channel] = constant;
            if (channel%1000==0) std::cout<<"Channel "<<channel<<" correction factor "<<fdQdxCalib[channel]<<std::endl;
        }
    }
    
    // Recover the baseline tool
    fBaseline  = art::make_tool<uboone_tool::IBaseline> (pset.get<fhicl::ParameterSet>("Baseline"));
    
    // Get signal shaping service.
    fSignalShaping = art::ServiceHandle<util::SignalShapingServiceMicroBooNE>();
    fFFT           = art::ServiceHandle<util::LArFFT>();
    
    return;
}
void ROIDeconvolution::Deconvolve(IROIFinder::Waveform&             waveform,
                                          raw::ChannelID_t                  channel,
                                          IROIFinder::CandidateROIVec&      roiVec,
                                          recob::Wire::RegionsOfInterest_t& ROIVec) const
{
    double                   deconNorm = fSignalShaping->GetDeconNorm();
    std::vector<geo::WireID> wids      = fGeometry->ChannelToWire(channel);
    size_t                   thePlane  = wids[0].Plane;
    
    // And now process them
    for(auto& roi : roiVec)
    {
        // First up: copy out the relevent ADC bins into the ROI holder
        size_t roiLen = roi.second - roi.first;
        
        // We want the deconvolution buffer size to be a power of 2 in length
        // to facilitate the FFT
        size_t deconSize = fFFTSize;
        
        while(1)
        {
            if (roiLen > deconSize) deconSize *= 2;
            else break;
        }
        
        // In theory, most ROI's are around the same size so this should mostly be a noop
        fSignalShaping->SetDecon(deconSize, channel);
        
        deconSize = fFFT->FFTSize();
        
        std::vector<float> holder(deconSize);
        
        // Extend the ROI to accommodate the extra bins for the FFT
        // The idea is to try to center the desired ROI in the buffer used by deconvolution
        size_t halfLeftOver = (deconSize - roiLen) / 2;               // Number bins either side of ROI
        size_t roiStart     = halfLeftOver;                           // Start in the buffer of the ROI
        size_t roiStop      = halfLeftOver + roiLen;                  // Stop in the buffer of the ROI
        int    firstOffset  = roi.first - halfLeftOver;               // Offset into the ADC vector of buffer start
        int    secondOffset = roi.second + halfLeftOver + roiLen % 2; // Offset into the ADC vector of buffer end
        
        // Check for the two edge conditions - starting before the ADC vector or running off the end
        // In either case we shift the actual roi within the FFT buffer
        // First is the case where we would be starting before the ADC vector
        if (firstOffset < 0)
        {
            roiStart     += firstOffset;  // remember that firstOffset is negative
            roiStop      += firstOffset;
            secondOffset -= firstOffset;
            firstOffset   = 0;
        }
        // Second is the case where we would overshoot the end
        else if (size_t(secondOffset) > waveform.size())
        {
            size_t overshoot = secondOffset - waveform.size();
            
            roiStart     += overshoot;
            roiStop      += overshoot;
            firstOffset  -= overshoot;
            secondOffset  = waveform.size();
        }
        
        // Fill the buffer and do the deconvolution
        std::copy(waveform.begin()+firstOffset, waveform.begin()+secondOffset, holder.begin());
        
        // Leon's algorithm for identifying charge collection on the afflicted v plane wires
        if (thePlane == 1 && wids[0].Wire > 1180 && wids[0].Wire < 1890)
        {
            std::vector<float>::iterator maxItr = std::max_element(holder.begin() + roiStart, holder.begin() + roiStop);
            std::vector<float>::iterator minItr = std::min_element(maxItr,                    holder.begin() + roiStop);
            
            if (std::distance(maxItr,minItr) <= 30)
            {
                float maxPulseHeight = *maxItr;
                float minPulseHeight = *minItr;
                
                if (maxPulseHeight >= 40. && std::fabs(minPulseHeight/maxPulseHeight) < 0.5)
                {
                    std::cout << "********> plane: " << thePlane << ", wire: " << wids[0].Wire << ", start: " << firstOffset << ", stop: " << secondOffset << std::endl;
                    std::cout << "          max to min distance: " << std::distance(maxItr,minItr) << ", max/min: " << maxPulseHeight << "/" << minPulseHeight << std::endl;
                    //                            deconChannel = 6000;
                }
            }
        }
        
        // Deconvolute the raw signal
        fSignalShaping->Deconvolute(channel,holder);
        
        // "normalize" the vector
        std::transform(holder.begin(),holder.end(),holder.begin(),[deconNorm](float& deconVal){return deconVal/deconNorm;});
        
        // Now we do the baseline determination and correct the ROI
        float base = fBaseline->GetBaseline(holder, channel, roiStart, roiLen);
        
        std::transform(holder.begin(),holder.end(),holder.begin(),[base](float& adcVal){return adcVal - base;});
        
        // Get rid of the leading and trailing "extra" bins needed to keep the FFT happy
        std::copy(holder.begin() + roiStart, holder.begin() + roiStop, holder.begin());
        
        holder.resize(roiLen);
        
        // apply wire-by-wire calibration
        if (fDodQdxCalib)
        {
            if(fdQdxCalib.find(channel) != fdQdxCalib.end())
            {
                float constant = fdQdxCalib.at(channel);
                
                for (size_t iholder = 0; iholder < holder.size(); ++iholder) holder[iholder] *= constant;
            }
        }
        
        //wes 23.12.2016 --- sum up the roi, and if it's very negative get rid of it
        float average_val = std::accumulate(holder.begin(),holder.end(),0.0) / holder.size();
        float min         = *std::min_element(holder.begin(),holder.end());
        float max         = *std::max_element(holder.begin(),holder.end());
        
        if ((max + average_val) > 1. && (average_val > fMinROIAverageTickThreshold ||  std::abs(min)<std::abs(max)))
        {
            // add the range into ROIVec
            ROIVec.add_range(roi.first, std::move(holder));
        }
        else
        {
            std::cout << "=======> Rejecting ROI due to average_val, plane: " << thePlane << ", wire: " << wids[0].Wire << ", val: " << average_val << ", min/max " << min << "/" << max << ", firstOffset: " << firstOffset << ", len: " << roiLen << std::endl;
        }
    } // loop over candidate roi's
    
    return;
}
    

    
void ROIDeconvolution::outputHistograms(art::TFileDirectory& histDir) const
{
    // It is assumed that the input TFileDirectory has been set up to group histograms into a common
    // folder at the calling routine's level. Here we create one more level of indirection to keep
    // histograms made by this tool separate.
/*
    std::string dirName = "ROIDeconvolutionPlane_" + std::to_string(fPlane);
 
    art::TFileDirectory dir = histDir.mkdir(dirName.c_str());
 
    auto const* detprop      = lar::providerFrom<detinfo::DetectorPropertiesService>();
    double      samplingRate = detprop->SamplingRate();
    double      numBins      = fROIDeconvolutionVec.size();
    double      maxFreq      = 500. / samplingRate;
    std::string histName     = "ROIDeconvolutionPlane_" + std::to_string(fPlane);
    
    TH1D*       hist         = dir.make<TH1D>(histName.c_str(), "ROIDeconvolution;Frequency(MHz)", numBins, 0., maxFreq);
    
    for(int bin = 0; bin < numBins; bin++)
    {
        double freq = maxFreq * double(bin + 0.5) / double(numBins);
        
        hist->Fill(freq, fROIDeconvolutionVec.at(bin).Re());
    }
*/
    
    return;
}
    
DEFINE_ART_CLASS_TOOL(ROIDeconvolution)
}