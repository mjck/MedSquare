/*=========================================================================

 Program:   MedSquare
 Module:    vtkmsqBruker2DSEQReader.cxx

 Copyright (c) Marcel P. Jackowski, Choukri Mekkaoui
 All rights reserved.
 See Copyright.txt or http://www.medsquare.org/copyright for details.

 This software is distributed WITHOUT ANY WARRANTY; without even
 the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the above copyright notice for more information.

 =========================================================================*/

#include "vtkmsqGDCMImageReader.h"

#include "vtkObjectFactory.h"
#include "vtkImageData.h"
#include "vtkMath.h"
#include "vtkPolyData.h"
#include "vtkCellArray.h"
#include "vtkPoints.h"
#include "vtkMedicalImageProperties.h"
#include "vtkStringArray.h"
#include "vtkPointData.h"
#include "vtkLookupTable.h"
#include "vtkWindowLevelLookupTable.h"
#include "vtkLookupTable16.h"
#include "vtkInformationVector.h"
#include "vtkInformation.h"
#include "vtkDemandDrivenPipeline.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkMatrix4x4.h"
#include "vtkUnsignedCharArray.h"
#include "vtkBitArray.h"

#include "gdcmImageReader.h"
#include "gdcmDataElement.h"
#include "gdcmByteValue.h"
#include "gdcmSwapper.h"
#include "gdcmUnpacker12Bits.h"
#include "gdcmRescaler.h"
#include "gdcmTrace.h"
#include "gdcmImageChangePlanarConfiguration.h"

#include <sstream>

/** \cond 0 */
//vtkCxxRevisionMacro(vtkmsqGDCMImageReader, "$Revision: 1.1 $")
vtkStandardNewMacro(vtkmsqGDCMImageReader)

vtkCxxSetObjectMacro(vtkmsqGDCMImageReader, MedicalImageProperties,
    vtkMedicalImageProperties)
/** \endcond */

/***********************************************************************************//**
 * 
 */
inline bool vtkmsqGDCMImageReader_IsCharTypeSigned()
{
#ifndef VTK_TYPE_CHAR_IS_SIGNED
  unsigned char uc = 255;
  return (*reinterpret_cast<char*>(&uc) < 0) ? true : false;
#else
  return VTK_TYPE_CHAR_IS_SIGNED;
#endif
}

/***********************************************************************************//**
 * 
 */
static gdcm::PixelFormat::ScalarType ComputePixelTypeFromFiles(const char *inputfilename,
    vtkStringArray *filenames, gdcm::Image const & imageref)
{
  gdcm::PixelFormat::ScalarType outputpt;
  outputpt = gdcm::PixelFormat::UNKNOWN;
  // there is a very subtle bug here. Let's imagine we have a collection of files
  // they can all have different Rescale Slope / Intercept. In this case we should:
  // 1. Make sure to read each Rescale Slope / Intercept individually
  // 2. Make sure to decide which Pixel Type to use using *all* slices:
  if (inputfilename)
  {
    const gdcm::Image &image = imageref;
    const gdcm::PixelFormat &pixeltype = image.GetPixelFormat();
    double shift = image.GetIntercept();
    double scale = image.GetSlope();

    gdcm::Rescaler r;
    r.SetIntercept(shift);
    r.SetSlope(scale);
    r.SetPixelFormat(pixeltype);
    outputpt = r.ComputeInterceptSlopePixelType();
  }
  else if (filenames && filenames->GetNumberOfValues() > 0)
  {
    std::set<gdcm::PixelFormat::ScalarType> pixeltypes;
    // FIXME a gdcm::Scanner would be much faster here:
    for (int i = 0; i < filenames->GetNumberOfValues(); ++i)
    {
      const char *filename = filenames->GetValue(i);
      gdcm::ImageReader reader;
      reader.SetFileName(filename);
      if (!reader.Read())
      {
        vtkGenericWarningMacro( "ImageReader failed: " << filename);
        return gdcm::PixelFormat::UNKNOWN;
      }
      const gdcm::Image &image = reader.GetImage();
      const gdcm::PixelFormat &pixeltype = image.GetPixelFormat();
      double shift = image.GetIntercept();
      double scale = image.GetSlope();

      gdcm::PixelFormat::ScalarType outputpt2 = pixeltype;
      gdcm::Rescaler r;
      r.SetIntercept(shift);
      r.SetSlope(scale);
      r.SetPixelFormat(pixeltype);
      outputpt2 = r.ComputeInterceptSlopePixelType();
      pixeltypes.insert(outputpt2);
    }
    if (pixeltypes.size() == 1)
    {
      // Ok easy case
      outputpt = *pixeltypes.begin();
    }
    else
    {
      // Hardcoded. If Pixel Type found is the maximum (as of PS 3.5 - 2008)
      // There is nothing bigger that FLOAT64
      if (pixeltypes.count(gdcm::PixelFormat::FLOAT64) != 0)
      {
        outputpt = gdcm::PixelFormat::FLOAT64;
      }
      else
      {
        // should I just take the biggest value ?
        // MM: I am not sure UINT16 and INT16 are really compatible
        // so taking the biggest value might not be the solution
        // In this case we could use INT32, but FLOAT64 also works...
        // oh well, let's just use FLOAT64 always.
        vtkGenericWarningMacro( "This may not always be optimized. Sorry");
        outputpt = gdcm::PixelFormat::FLOAT64;
      }
    }
  }
  else
  {
    assert( 0);
    // I do not think this is possible
  }

  return outputpt;
}

/***********************************************************************************//**
 * 
 */
template<class T>
inline unsigned long vtkImageDataGetTypeSize(T*, int a = 0, int b = 0)
{
  (void) a;
  (void) b;
  return sizeof(T);
}

/***********************************************************************************//**
 * 
 */
void InPlaceYFlipImage(vtkImageData* data)
{
  unsigned long outsize = data->GetNumberOfScalarComponents();
  //int *dext = data->GetWholeExtent();
  int *dext = data->GetExtent();
  if (dext[1] == dext[0] && dext[0] == 0)
    return;

  // Multiply by the number of bytes per scalar
  switch (data->GetScalarType())
  {
#if (VTK_MAJOR_VERSION >= 5) || ( VTK_MAJOR_VERSION == 4 && VTK_MINOR_VERSION > 5 )
    case VTK_BIT:
    {
      outsize /= 8;
    }
      ;
      break;
    vtkTemplateMacro( outsize *= vtkImageDataGetTypeSize(static_cast<VTK_TT*>(0)));
#else
      case VTK_BIT:
      { outsize /= 8;}; break;
      vtkTemplateMacro3(
          outsize *= vtkImageDataGetTypeSize, static_cast<VTK_TT*>(0), 0, 0
      );
#endif
    default:
      assert(0);
  }
  outsize *= (dext[1] - dext[0] + 1);
  char * ref = static_cast<char*>(data->GetScalarPointer());
  char * pointer = static_cast<char*>(data->GetScalarPointer());
  assert( pointer);

  char *line = new char[outsize];

  for (int j = dext[4]; j <= dext[5]; ++j)
  {
    char *start = pointer;
    assert( start == ref + j * outsize * (dext[3] - dext[2] + 1));
    // Swap two-lines at a time
    // when Rows is odd number (359) then dext[3] == 178
    // so we should avoid copying the line right in the center of the image
    // since memcpy does not like copying on itself...
    for (int i = dext[2]; i < (dext[3] + 1) / 2; ++i)
    {
      // image:
      char * end = start + (dext[3] - i) * outsize;
      assert( (end - pointer) >= (int)outsize);
      memcpy(line, end, outsize); // duplicate line
      memcpy(end, pointer, outsize);
      memcpy(pointer, line, outsize);
      pointer += outsize;
    }
    // because the for loop iterated only over 1/2 all lines, skip to the next slice:
    assert( dext[2] == 0);
    pointer += (dext[3] + 1 - (dext[3] + 1) / 2) * outsize;
  }
  // Did we reach the end ?
  assert( pointer == ref + (dext[5]-dext[4]+1)*(dext[3]-dext[2]+1)*outsize);
  delete[] line;
}

/***********************************************************************************//**
 * 
 */
vtkmsqGDCMImageReader::vtkmsqGDCMImageReader()
{
  // vtkDataArray has an internal vtkLookupTable why not used it ?
  // vtkMedicalImageProperties is in the parent class
  this->DirectionCosines = vtkMatrix4x4::New();
  this->DirectionCosines->Identity();

  this->MedicalImageProperties = vtkMedicalImageProperties::New();
  this->ImageFormat = 0; // INVALID

  memset(this->ImagePositionPatient, 0, 3 * sizeof(double));
  memset(this->ImageOrientationPatient, 0, 6 * sizeof(double));

  this->Shift = 0.;
  this->Scale = 1.;

  this->PlanarConfiguration = 0;
  this->LossyFlag = 0;

  // DirectionCosine was added after 5.2
  this->MedicalImageProperties->SetDirectionCosine(1, 0, 0, 0, 1, 0);
  this->SetImageOrientationPatient(1, 0, 0, 0, 1, 0);

  this->ForceRescale = 0;
}

/***********************************************************************************//**
 * 
 */
vtkmsqGDCMImageReader::~vtkmsqGDCMImageReader()
{
  this->DirectionCosines->Delete();
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::CanReadFile(const char* fname)
{
  gdcm::ImageReader reader;
  reader.SetFileName(fname);
  if (!reader.Read())
  {
    return 0;
  }
  // 3 means: I might be able to read...
  return 3;
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::ProcessRequest(vtkInformation* request,
    vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // generate the data
  if (request->Has(vtkDemandDrivenPipeline::REQUEST_DATA()))
  {
    return this->RequestData(request, inputVector, outputVector);
  }

  // execute information
  if (request->Has(vtkDemandDrivenPipeline::REQUEST_INFORMATION()))
  {
    return this->RequestInformation(request, inputVector, outputVector);
  }

  return this->Superclass::ProcessRequest(request, inputVector, outputVector);
}

/***********************************************************************************//**
 * 
 */
inline const char *GetStringValueFromTag(const gdcm::Tag& t, const gdcm::DataSet& ds)
{
  static std::string buffer;
  buffer = ""; // cleanup previous call

  if (ds.FindDataElement(t))
  {
    const gdcm::DataElement& de = ds.GetDataElement(t);
    const gdcm::ByteValue *bv = de.GetByteValue();
    if (bv) // Can be Type 2
    {
      buffer = std::string(bv->GetPointer(), bv->GetLength());
      // Will be padded with at least one \0
    }
  }

  // Since return is a const char* the very first \0 will be considered
  return buffer.c_str();
}

/***********************************************************************************//**
 * 
 */
void vtkmsqGDCMImageReader::FillMedicalImageInformation(const gdcm::ImageReader &reader)
{
  const gdcm::File &file = reader.GetFile();
  const gdcm::DataSet &ds = file.GetDataSet();

  // $ grep "vtkSetString\|DICOM" vtkMedicalImageProperties.h
  // For ex: DICOM (0010,0010) = DOE,JOHN
  this->MedicalImageProperties->SetPatientName(
      GetStringValueFromTag(gdcm::Tag(0x0010, 0x0010), ds));
  // For ex: DICOM (0010,0020) = 1933197
  this->MedicalImageProperties->SetPatientID(
      GetStringValueFromTag(gdcm::Tag(0x0010, 0x0020), ds));
  // For ex: DICOM (0010,1010) = 031Y
  this->MedicalImageProperties->SetPatientAge(
      GetStringValueFromTag(gdcm::Tag(0x0010, 0x1010), ds));
  // For ex: DICOM (0010,0040) = M
  this->MedicalImageProperties->SetPatientSex(
      GetStringValueFromTag(gdcm::Tag(0x0010, 0x0040), ds));
  // For ex: DICOM (0010,0030) = 19680427
  this->MedicalImageProperties->SetPatientBirthDate(
      GetStringValueFromTag(gdcm::Tag(0x0010, 0x0030), ds));
  //#if ( VTK_MAJOR_VERSION == 5 && VTK_MINOR_VERSION > 0 )
  // For ex: DICOM (0008,0020) = 20030617
  this->MedicalImageProperties->SetStudyDate(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0020), ds));
  //#endif
  // For ex: DICOM (0008,0022) = 20030617
  this->MedicalImageProperties->SetAcquisitionDate(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0022), ds));
  //#if ( VTK_MAJOR_VERSION == 5 && VTK_MINOR_VERSION > 0 )
  // For ex: DICOM (0008,0030) = 162552.0705 or 230012, or 0012
  this->MedicalImageProperties->SetStudyTime(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0030), ds));
  //#endif
  // For ex: DICOM (0008,0032) = 162552.0705 or 230012, or 0012
  this->MedicalImageProperties->SetAcquisitionTime(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0032), ds));
  // For ex: DICOM (0008,0023) = 20030617
  this->MedicalImageProperties->SetImageDate(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0023), ds));
  // For ex: DICOM (0008,0033) = 162552.0705 or 230012, or 0012
  this->MedicalImageProperties->SetImageTime(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0033), ds));
  // For ex: DICOM (0020,0013) = 1
  this->MedicalImageProperties->SetImageNumber(
      GetStringValueFromTag(gdcm::Tag(0x0020, 0x0013), ds));
  // For ex: DICOM (0020,0011) = 902
  this->MedicalImageProperties->SetSeriesNumber(
      GetStringValueFromTag(gdcm::Tag(0x0020, 0x0011), ds));
  // For ex: DICOM (0008,103e) = SCOUT
  this->MedicalImageProperties->SetSeriesDescription(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x103e), ds));
  // For ex: DICOM (0020,0010) = 37481
  this->MedicalImageProperties->SetStudyID(
      GetStringValueFromTag(gdcm::Tag(0x0020, 0x0010), ds));
  // For ex: DICOM (0008,1030) = BRAIN/C-SP/FACIAL
  this->MedicalImageProperties->SetStudyDescription(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x1030), ds));
  // For ex: DICOM (0008,0060)= CT
  this->MedicalImageProperties->SetModality(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0060), ds));
  // For ex: DICOM (0008,0070) = Siemens
  this->MedicalImageProperties->SetManufacturer(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0070), ds));
  // For ex: DICOM (0008,1090) = LightSpeed QX/i
  this->MedicalImageProperties->SetManufacturerModelName(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x1090), ds));
  // For ex: DICOM (0008,1010) = LSPD_OC8
  this->MedicalImageProperties->SetStationName(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x1010), ds));
  // For ex: DICOM (0008,0080) = FooCity Medical Center
  this->MedicalImageProperties->SetInstitutionName(
      GetStringValueFromTag(gdcm::Tag(0x0008, 0x0080), ds));
  // For ex: DICOM (0018,1210) = Bone
  this->MedicalImageProperties->SetConvolutionKernel(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x1210), ds));
  // For ex: DICOM (0018,0050) = 0.273438
  this->MedicalImageProperties->SetSliceThickness(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x0050), ds));
  // For ex: DICOM (0018,0060) = 120
  this->MedicalImageProperties->SetKVP(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x0060), ds));
  // For ex: DICOM (0018,1120) = 15
  this->MedicalImageProperties->SetGantryTilt(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x1120), ds));
  // For ex: DICOM (0018,0081) = 105
  this->MedicalImageProperties->SetEchoTime(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x0081), ds));
  // For ex: DICOM (0018,0091) = 35
  this->MedicalImageProperties->SetEchoTrainLength(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x0091), ds));
  // For ex: DICOM (0018,0080) = 2040
  this->MedicalImageProperties->SetRepetitionTime(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x0080), ds));
  // For ex: DICOM (0018,1150) = 5
  this->MedicalImageProperties->SetExposureTime(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x1150), ds));
  // For ex: DICOM (0018,1151) = 400
  this->MedicalImageProperties->SetXRayTubeCurrent(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x1151), ds));
  // For ex: DICOM (0018,1152) = 114
  this->MedicalImageProperties->SetExposure(
      GetStringValueFromTag(gdcm::Tag(0x0018, 0x1152), ds));

  // (0028,1050) DS [   498\  498]                           #  12, 2 WindowCenter
  // (0028,1051) DS [  1063\ 1063]                           #  12, 2 WindowWidth
  gdcm::Tag twindowcenter(0x0028, 0x1050);
  gdcm::Tag twindowwidth(0x0028, 0x1051);
  if (ds.FindDataElement(twindowcenter) && ds.FindDataElement(twindowwidth))
  {
    const gdcm::DataElement& windowcenter = ds.GetDataElement(twindowcenter);
    const gdcm::DataElement& windowwidth = ds.GetDataElement(twindowwidth);
    const gdcm::ByteValue *bvwc = windowcenter.GetByteValue();
    const gdcm::ByteValue *bvww = windowwidth.GetByteValue();
    if (bvwc && bvww) // Can be Type 2
    {
      gdcm::Element<gdcm::VR::DS, gdcm::VM::VM1_n> elwc;
      std::stringstream ss1;
      std::string swc = std::string(bvwc->GetPointer(), bvwc->GetLength());
      ss1.str(swc);
      gdcm::VR vr = gdcm::VR::DS;
      unsigned int vrsize = vr.GetSizeof();
      unsigned int count = gdcm::VM::GetNumberOfElementsFromArray(swc.c_str(),
          swc.size());
      elwc.SetLength(count * vrsize);
      elwc.Read(ss1);
      std::stringstream ss2;
      std::string sww = std::string(bvww->GetPointer(), bvww->GetLength());
      ss2.str(sww);
      gdcm::Element<gdcm::VR::DS, gdcm::VM::VM1_n> elww;
      elww.SetLength(count * vrsize);
      elww.Read(ss2);
      for (unsigned int i = 0; i < elwc.GetLength(); ++i)
      {
        this->MedicalImageProperties->AddWindowLevelPreset(elww.GetValue(i),
            elwc.GetValue(i));
      }
    }
  }
  gdcm::Tag twindowexplanation(0x0028, 0x1055);
  if (ds.FindDataElement(twindowexplanation))
  {
    const gdcm::DataElement& windowexplanation = ds.GetDataElement(twindowexplanation);
    const gdcm::ByteValue *bvwe = windowexplanation.GetByteValue();
    if (bvwe) // Can be Type 2
    {
      unsigned int n = this->MedicalImageProperties->GetNumberOfWindowLevelPresets();
      gdcm::Element<gdcm::VR::LO, gdcm::VM::VM1_n> elwe; // window explanation
      gdcm::VR vr = gdcm::VR::LO;
      std::stringstream ss;
      ss.str("");
      std::string swe = std::string(bvwe->GetPointer(), bvwe->GetLength());
      unsigned int count = gdcm::VM::GetNumberOfElementsFromArray(swe.c_str(),
          swe.size());
      (void) count;
      elwe.SetLength(count * vr.GetSizeof());
      ss.str(swe);
      elwe.Read(ss);
      unsigned int c = std::min(n, count);
      for (unsigned int i = 0; i < c; ++i)
      {
        this->MedicalImageProperties->SetNthWindowLevelPresetComment(i,
            elwe.GetValue(i).c_str());
      }
    }
  }
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::RequestInformationCompat()
{
  // Let's read the first file :
  const char *filename;
  if (this->FileName)
  {
    filename = this->FileName;
  }
  else if (this->FileNames && this->FileNames->GetNumberOfValues() > 0)
  {
    filename = this->FileNames->GetValue(0);
  }
  else
  {
    // hey! I need at least one file to schew on !
    vtkErrorMacro( "You did not specify any filenames");
    return 0;
  }
  gdcm::ImageReader reader;
  reader.SetFileName(filename);
  if (!reader.Read())
  {
    vtkErrorMacro( "ImageReader failed on " << filename);
    return 0;
  }
  const gdcm::Image &image = reader.GetImage();
  this->LossyFlag = image.IsLossy();
  const unsigned int *dims = image.GetDimensions();

  // Set the Extents.
  assert( image.GetNumberOfDimensions() >= 2);
  this->DataExtent[0] = 0;
  this->DataExtent[1] = dims[0] - 1;
  this->DataExtent[2] = 0;
  this->DataExtent[3] = dims[1] - 1;
  if (image.GetNumberOfDimensions() == 2)
  {
    // This is just so much painful to deal with DICOM / VTK
    // they simply assume that number of file is equal to the dimension
    // of the last axe (see vtkImageReader2::SetFileNames )
    if (this->FileNames && this->FileNames->GetNumberOfValues() > 1)
    {
      this->DataExtent[4] = 0;
    }
    else
    {
      this->DataExtent[4] = 0;
      this->DataExtent[5] = 0;
    }
  }
  else
  {
    assert( image.GetNumberOfDimensions() == 3);
    this->FileDimensionality = 3;
    this->DataExtent[4] = 0;
    this->DataExtent[5] = dims[2] - 1;
  }

  const double *spacing = image.GetSpacing();
  if (spacing)
  {
    this->DataSpacing[0] = spacing[0];
    this->DataSpacing[1] = spacing[1];
    this->DataSpacing[2] = image.GetSpacing(2);
  }

  const double *origin = image.GetOrigin();
  if (origin)
  {
    this->ImagePositionPatient[0] = image.GetOrigin(0);
    this->ImagePositionPatient[1] = image.GetOrigin(1);
    this->ImagePositionPatient[2] = image.GetOrigin(2);
  }

  const double *dircos = image.GetDirectionCosines();
  if (dircos)
  {
    this->DirectionCosines->SetElement(0, 0, dircos[0]);
    this->DirectionCosines->SetElement(1, 0, dircos[1]);
    this->DirectionCosines->SetElement(2, 0, dircos[2]);
    this->DirectionCosines->SetElement(3, 0, 0);
    this->DirectionCosines->SetElement(0, 1, dircos[3]);
    this->DirectionCosines->SetElement(1, 1, dircos[4]);
    this->DirectionCosines->SetElement(2, 1, dircos[5]);
    this->DirectionCosines->SetElement(3, 1, 0);
    double dircosz[3];
    vtkMath::Cross(dircos, dircos + 3, dircosz);
    this->DirectionCosines->SetElement(0, 2, dircosz[0]);
    this->DirectionCosines->SetElement(1, 2, dircosz[1]);
    this->DirectionCosines->SetElement(2, 2, dircosz[2]);
    this->DirectionCosines->SetElement(3, 2, 0);

    for (int i = 0; i < 6; ++i)
      this->ImageOrientationPatient[i] = dircos[i];
#if ( VTK_MAJOR_VERSION == 5 && VTK_MINOR_VERSION > 2 )
    this->MedicalImageProperties->SetDirectionCosine(this->ImageOrientationPatient);
#endif
  }
  // Apply transform:
  if (dircos && origin)
  {
    if (this->FileLowerLeft)
    {
      // Since we are not doing the VTK Y-flipping operation, Origin and Image Position (Patient)
      // are the same:
      this->DataOrigin[0] = origin[0];
      this->DataOrigin[1] = origin[1];
      this->DataOrigin[2] = origin[2];
    }
    else
    {
      // We are doing the Y-flip:
      // translate Image Position (Patient) along the Y-vector of the Image Orientation (Patient):
      // Step 1: Compute norm of translation vector:
      // Because position is in the center of the pixel, we need to substract 1 to the dimY:
      assert( dims[1] >=1);
      double norm = (dims[1] - 1) * this->DataSpacing[1];
      // Step 2: translate:
      this->DataOrigin[0] = origin[0] + norm * dircos[3 + 0];
      this->DataOrigin[1] = origin[1] + norm * dircos[3 + 1];
      this->DataOrigin[2] = origin[2] + norm * dircos[3 + 2];
    }
  }
  // Need to set the rest to 0 ???

  const gdcm::PixelFormat &pixeltype = image.GetPixelFormat();
  this->Shift = image.GetIntercept();
  this->Scale = image.GetSlope();

  gdcm::PixelFormat::ScalarType outputpt = ComputePixelTypeFromFiles(this->FileName,
      this->FileNames, image);

  this->ForceRescale = 0; // always reset this thing
  if (pixeltype != outputpt && pixeltype.GetBitsAllocated() != 12)
  {
    this->ForceRescale = 1;
  }

  switch (outputpt)
  {
    case gdcm::PixelFormat::INT8:
#if (VTK_MAJOR_VERSION >= 5) || ( VTK_MAJOR_VERSION == 4 && VTK_MINOR_VERSION > 5 )
      this->DataScalarType = VTK_SIGNED_CHAR;
#else
      if( !vtkmsqGDCMImageReader_IsCharTypeSigned() )
      {
        vtkErrorMacro( "Output Pixel Type will be incorrect, go get a newer VTK version" );
      }
      this->DataScalarType = VTK_CHAR;
#endif
      break;
    case gdcm::PixelFormat::UINT8:
      this->DataScalarType = VTK_UNSIGNED_CHAR;
      break;
    case gdcm::PixelFormat::INT16:
      this->DataScalarType = VTK_SHORT;
      break;
    case gdcm::PixelFormat::UINT16:
      this->DataScalarType = VTK_UNSIGNED_SHORT;
      break;
      // RT / SC have 32bits
    case gdcm::PixelFormat::INT32:
      this->DataScalarType = VTK_INT;
      break;
    case gdcm::PixelFormat::UINT32:
      this->DataScalarType = VTK_UNSIGNED_INT;
      break;
    case gdcm::PixelFormat::INT12:
      this->DataScalarType = VTK_SHORT;
      break;
    case gdcm::PixelFormat::UINT12:
      this->DataScalarType = VTK_UNSIGNED_SHORT;
      break;
      //case gdcm::PixelFormat::FLOAT16: // TODO
    case gdcm::PixelFormat::FLOAT32:
      this->DataScalarType = VTK_FLOAT;
      break;
    case gdcm::PixelFormat::FLOAT64:
      this->DataScalarType = VTK_DOUBLE;
      break;
    case gdcm::PixelFormat::SINGLEBIT:
      this->DataScalarType = VTK_BIT;
      break;
    default:
      vtkErrorMacro(
          "Do not support this Pixel Type: " << pixeltype.GetScalarType() << " with " << outputpt);
      return 0;
  }
  this->NumberOfScalarComponents = pixeltype.GetSamplesPerPixel();

  // Ok let's fill in the 'extra' info:
  this->FillMedicalImageInformation(reader);

  return 1;
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::RequestInformation(vtkInformation *request,
    vtkInformationVector **inputVector, vtkInformationVector *outputVector)
{
  (void) request;
  (void) inputVector;
  int res = RequestInformationCompat();
  if (!res)
  {
    vtkErrorMacro( "RequestInformationCompat failed: " << res);
    return 0;
  }

  int numvol = 1;
  this->SetNumberOfOutputPorts(numvol);

  // Allocate !
  if (!this->GetOutput(0))
  {
    vtkImageData *img = vtkImageData::New();
    this->GetExecutive()->SetOutputData(0, img);
    img->Delete();
  }
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  outInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), this->DataExtent, 6);
  outInfo->Set(vtkDataObject::SPACING(), this->DataSpacing, 3);
  outInfo->Set(vtkDataObject::ORIGIN(), this->DataOrigin, 3);
  vtkDataObject::SetPointDataActiveScalarInfo(outInfo, this->DataScalarType,
      this->NumberOfScalarComponents);

  return 1;
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::LoadSingleFile(const char *filename, char *pointer,
    unsigned long &outlen)
{
  int *dext = this->GetDataExtent();
  vtkImageData *data = this->GetOutput(0);

  gdcm::ImageReader reader;
  reader.SetFileName(filename);
  if (!reader.Read())
  {
    vtkErrorMacro( "ImageReader failed: " << filename);
    return 0;
  }
  gdcm::Image &image = reader.GetImage();
  this->LossyFlag = image.IsLossy();
  //VTK does not cope with Planar Configuration, so let's schew the work to please it
  assert( this->PlanarConfiguration == 0 || this->PlanarConfiguration == 1);
  // Store the PlanarConfiguration before inverting it !
  this->PlanarConfiguration = image.GetPlanarConfiguration();
  if (image.GetPlanarConfiguration() == 1)
  {
    gdcm::ImageChangePlanarConfiguration icpc;
    icpc.SetInput(image);
    icpc.SetPlanarConfiguration(0);
    icpc.Change();
    image = icpc.GetOutput();
  }

  const gdcm::PixelFormat &pixeltype = image.GetPixelFormat();
  assert( image.GetNumberOfDimensions() == 2 || image.GetNumberOfDimensions() == 3);
  /*const*/
  unsigned long len = image.GetBufferLength();
  outlen = len;
  this->Shift = image.GetIntercept();
  this->Scale = image.GetSlope();

  if ((this->Scale != 1.0 || this->Shift != 0.0) || this->ForceRescale)
  {
    assert( pixeltype.GetSamplesPerPixel() == 1);
    gdcm::Rescaler r;
    r.SetIntercept(Shift); // FIXME
    r.SetSlope(Scale); // FIXME
    gdcm::PixelFormat::ScalarType targetpixeltype = gdcm::PixelFormat::UNKNOWN;
    int scalarType = data->GetScalarType();
    switch (scalarType)
    {
      case VTK_CHAR:
        if (vtkmsqGDCMImageReader_IsCharTypeSigned())
          targetpixeltype = gdcm::PixelFormat::INT8;
        else
          targetpixeltype = gdcm::PixelFormat::UINT8;
        break;
#if (VTK_MAJOR_VERSION >= 5) || ( VTK_MAJOR_VERSION == 4 && VTK_MINOR_VERSION > 5 )
      case VTK_SIGNED_CHAR:
        targetpixeltype = gdcm::PixelFormat::INT8;
        break;
#endif
      case VTK_UNSIGNED_CHAR:
        targetpixeltype = gdcm::PixelFormat::UINT8;
        break;
      case VTK_SHORT:
        targetpixeltype = gdcm::PixelFormat::INT16;
        break;
      case VTK_UNSIGNED_SHORT:
        targetpixeltype = gdcm::PixelFormat::UINT16;
        break;
      case VTK_INT:
        targetpixeltype = gdcm::PixelFormat::INT32;
        break;
      case VTK_UNSIGNED_INT:
        targetpixeltype = gdcm::PixelFormat::UINT32;
        break;
      case VTK_FLOAT:
        targetpixeltype = gdcm::PixelFormat::FLOAT32;
        break;
      case VTK_DOUBLE:
        targetpixeltype = gdcm::PixelFormat::FLOAT64;
        break;
      case VTK_BIT:
        targetpixeltype = gdcm::PixelFormat::SINGLEBIT;
        break;
      default:
        vtkErrorMacro( "Do not support this Pixel Type: " << scalarType);
        assert( 0);
        return 0;
    }
    r.SetTargetPixelType(targetpixeltype);

    r.SetUseTargetPixelType(true);
    r.SetPixelFormat(pixeltype);
    char * copy = new char[len];
    image.GetBuffer(copy);
    if (!r.Rescale(pointer, copy, len))
    {
      vtkErrorMacro( "Could not Rescale");
      // problem with gdcmData/3E768EB7.dcm
      return 0;
    }
    delete[] copy;
    // WARNING: sizeof(Real World Value) != sizeof(Stored Pixel)
    outlen = data->GetScalarSize() * data->GetNumberOfPoints() / data->GetDimensions()[2];
    assert( data->GetNumberOfScalarComponents() == 1);
  }
  else
  {
    image.GetBuffer(pointer);
  }

  // Do the LUT
  if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::PALETTE_COLOR)
  {
    this->ImageFormat = VTK_LOOKUP_TABLE;
    const gdcm::LookupTable &lut = image.GetLUT();
    if (lut.GetBitSample() == 8)
    {
      vtkLookupTable *vtklut = vtkLookupTable::New();
      vtklut->SetNumberOfTableValues(256);
      // SOLVED: GetPointer(0) is skrew up, need to replace it with WritePointer(0,4) ...
      if (!lut.GetBufferAsRGBA(vtklut->WritePointer(0, 4)))
      {
        vtkWarningMacro( "Could not get values from LUT");
        return 0;
      }
      vtklut->SetRange(0, 255);
      data->GetPointData()->GetScalars()->SetLookupTable(vtklut);
      vtklut->Delete();
    }
    else
    {
#if (VTK_MAJOR_VERSION >= 5)
      assert( lut.GetBitSample() == 16);
      vtkLookupTable16 *vtklut = vtkLookupTable16::New();
      vtklut->SetNumberOfTableValues(256 * 256);
      // SOLVED: GetPointer(0) is skrew up, need to replace it with WritePointer(0,4) ...
      if (!lut.GetBufferAsRGBA((unsigned char*) vtklut->WritePointer(0, 4)))
      {
        vtkWarningMacro( "Could not get values from LUT");
        return 0;
      }
      vtklut->SetRange(0, 256 * 256 - 1);
      data->GetPointData()->GetScalars()->SetLookupTable(vtklut);
      vtklut->Delete();
#else
      vtkWarningMacro( "Unhandled" );
#endif
    }
  }
  else if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::MONOCHROME1)
  {
    this->ImageFormat = VTK_INVERSE_LUMINANCE;
    vtkWindowLevelLookupTable *vtklut = vtkWindowLevelLookupTable::New();
    // Technically we could also use the first of the Window Width / Window Center
    // oh well, if they are missing let's just compute something:
    int64_t min = pixeltype.GetMin();
    int64_t max = pixeltype.GetMax();
    vtklut->SetWindow(max - min);
    vtklut->SetLevel(0.5 * (max + min));
    vtklut->InverseVideoOn();
    data->GetPointData()->GetScalars()->SetLookupTable(vtklut);
    vtklut->Delete();
  }
  else if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::YBR_FULL_422)
  {
    this->ImageFormat = VTK_YBR;
  }
  else if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::YBR_FULL)
  {
    this->ImageFormat = VTK_YBR;
  }
  else if (image.GetPhotometricInterpretation() == gdcm::PhotometricInterpretation::RGB)
  {
    this->ImageFormat = VTK_RGB;
  }
  else if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::MONOCHROME2)
  {
    this->ImageFormat = VTK_LUMINANCE;
  }
  else if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::YBR_RCT)
  {
    this->ImageFormat = VTK_RGB;
  }
  else if (image.GetPhotometricInterpretation()
      == gdcm::PhotometricInterpretation::YBR_ICT)
  {
    this->ImageFormat = VTK_RGB;
  }
  else if (image.GetPhotometricInterpretation() == gdcm::PhotometricInterpretation::CMYK)
  {
    this->ImageFormat = VTK_CMYK;
  }
  else if (image.GetPhotometricInterpretation() == gdcm::PhotometricInterpretation::ARGB)
  {
    this->ImageFormat = VTK_RGBA;
  }
  else
  {
    // HSV / CMYK ???
    // let's just give up for now
    vtkErrorMacro(
        "Does not handle: " << image.GetPhotometricInterpretation().GetString());
  }

  long outsize;
  if (data->GetScalarType() == VTK_BIT)
  {
    outsize = (dext[1] - dext[0] + 1) / 8;
  }
  else
  {
    outsize = pixeltype.GetPixelSize() * (dext[1] - dext[0] + 1);
  }

  if (this->FileName)
    assert( (unsigned long)outsize * (dext[3] - dext[2]+1) * (dext[5]-dext[4]+1) == len);

  return 1; // success
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::RequestDataCompat()
{
  vtkImageData *output = this->GetOutput(0);
  output->GetPointData()->GetScalars()->SetName("GDCMImage");

  int outExt[6];
  output->GetExtent(outExt);

  char * pointer = static_cast<char*>(output->GetScalarPointerForExtent(outExt));
  if (this->FileName)
  {
    const char *filename = this->FileName;
    unsigned long len;
    int load = this->LoadSingleFile(filename, pointer, len);
    (void) len;
    if (!load)
    {
      // FIXME: I need to fill the buffer with 0, shouldn't I ?
      return 0;
    }
  }
  else if (this->FileNames && this->FileNames->GetNumberOfValues() >= 1)
  {
    // Load each 2D files
    int *dext = this->GetDataExtent();
    // HACK: len is moved out of the loop so that when file > 1 start failing we can still know
    // the len of the buffer...technically all files should have the same len (not checked for now)
    unsigned long len = 0;
    for (int j = dext[4]; j <= dext[5]; ++j)
    {
      assert( j >= 0 && j <= this->FileNames->GetNumberOfValues());
      const char *filename = this->FileNames->GetValue(j);
      int load = this->LoadSingleFile(filename, pointer, len);
      if (!load)
      {
        // hum... we could not read this file within the series, let's just fill
        // the slice with 0 value, hopefully this should be the right thing to do
        memset(pointer, 0, len);
      }assert( len);
      pointer += len;
      this->UpdateProgress((double) (j - dext[4]) / (dext[5] - dext[4]));
    }
  }
  else
  {
    return 0;
  }
  // Y-flip image
  if (!this->FileLowerLeft)
  {
    InPlaceYFlipImage(this->GetOutput(0));
  }

  return 1;
}

/***********************************************************************************//**
 * 
 */
int vtkmsqGDCMImageReader::RequestData(vtkInformation *vtkNotUsed(request),
    vtkInformationVector **vtkNotUsed(inputVector), vtkInformationVector *outputVector)
{
  (void) outputVector;

  // Make sure the output dimension is OK, and allocate its scalars

  for (int i = 0; i < this->GetNumberOfOutputPorts(); ++i)
  {
    vtkInformation *outInfo = outputVector->GetInformationObject(i);
    vtkImageData *data = static_cast<vtkImageData *>(outInfo->Get(vtkDataObject::DATA_OBJECT()));
    if (data) {
      int extent[6];
      outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(), extent);
      this->AllocateOutputData(data, outInfo, extent);
    }
  }
  int res = RequestDataCompat();
  return res;
}

/***********************************************************************************//**
 * 
 */
void vtkmsqGDCMImageReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

