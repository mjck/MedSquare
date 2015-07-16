#Script file to set paths for medsquare base distribution
MSQROOT=~/medsquare/deps

if [ -n $LD_LIBRARY_PATH ]; then
    echo ""
else
    LD_LIBRARY_PATH=/usr/local/lib
fi


# ------------------- Name libraries ---------------------
VTK_DIR=${MSQROOT}/vtk510/lib/vtk-5.10;   export VTK_DIR
ITK_DIR=${MSQROOT}/itk320/lib/InsightToolkit; export ITK_DIR
GDCM_DIR=${MSQROOT}/gdcm226/lib/gdcm-2.0; export GDCM_DIR

PATH=${MSQROOT}/cmake28/bin:${PATH}
PATH=${MSQROOT}/vtk510/bin:${PATH}
PATH=${MSQROOT}/gdcm226/bin:${PATH}
export PATH


LD_LIBRARY_PATH=${MSQROOT}/vtk510/lib/vtk-5.10:${LD_LIBRARY_PATH}
LD_LIBRARY_PATH=${MSQROOT}/itk320/lib/InsightToolkit:${LD_LIBRARY_PATH}
LD_LIBRARY_PATH=${MSQROOT}/gdcm226/lib:${LD_LIBRARY_PATH}
export LD_LIBRARY_PATH

DYLD_LIBRARY_PATH=${MSQROOT}/vtk510/lib/vtk-5.10:${DYLD_LIBRARY_PATH}
DYLD_LIBRARY_PATH=${MSQROOT}/itk320/lib/InsightToolkit:${DYLD_LIBRARY_PATH}
DYLD_LIBRARY_PATH=${MSQROOT}/gdcm226/lib:${DYLD_LIBRARY_PATH}
export DYLD_LIBRARY_PATH

# ----------------------------------------------------------
echo "MedSquare is now in your path"
echo "Key Components"

which cmake
which vtk
which gdcminfo

echo "ITK in ${ITK_DIR}"






