/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2013 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "pointConstraints.H"
#include "pointFields.H"
#include "valuePointPatchFields.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

template<class Type, class CombineOp>
void pointConstraints::syncUntransformedData
(
    const polyMesh& mesh,
    gpuList<Type>& pointData,
    const CombineOp& cop
)
{
    // Transfer onto coupled patch
    const globalMeshData& gmd = mesh.globalData();
    const indirectPrimitivePatch& cpp = gmd.coupledPatch();
    const labelgpuList& meshPoints = cpp.getMeshPoints();

    const mapDistribute& slavesMap = gmd.globalCoPointSlavesMap();
    const labelListList& slaves = gmd.globalCoPointSlaves();

    List<Type> elems(slavesMap.constructSize());
    thrust::copy
    (
        thrust::make_permutation_iterator
        (
            pointData.begin(),
            meshPoints.begin()
        ),
        thrust::make_permutation_iterator
        (
            pointData.begin(),
            meshPoints.end()
        ),
        elems.begin()
    );

    // Pull slave data onto master. No need to update transformed slots.
    slavesMap.distribute(elems, false);

    // Combine master data with slave data
    forAll(slaves, i)
    {
        Type& elem = elems[i];

        const labelList& slavePoints = slaves[i];

        // Combine master with untransformed slave data
        forAll(slavePoints, j)
        {
            cop(elem, elems[slavePoints[j]]);
        }

        // Copy result back to slave slots
        forAll(slavePoints, j)
        {
            elems[slavePoints[j]] = elem;
        }
    }

    // Push slave-slot data back to slaves
    slavesMap.reverseDistribute(elems.size(), elems, false);

    // Extract back onto mesh
    thrust::copy
    (
        elems.begin(),
        elems.begin()+meshPoints.size(),
        thrust::make_permutation_iterator
        (
            pointData.begin(),
            meshPoints.begin()
        )
    );
/*
    forAll(meshPoints, i)
    {
        pointData[meshPoints[i]] = elems[i];
    }
*/
}


template<class Type>
void pointConstraints::setPatchFields
(
    GeometricField<Type, pointPatchField, pointMesh>& pf
)
{
    forAll(pf.boundaryField(), patchI)
    {
        pointPatchField<Type>& ppf = pf.boundaryField()[patchI];

        if (isA<valuePointPatchField<Type> >(ppf))
        {
            refCast<valuePointPatchField<Type> >(ppf) =
                ppf.patchInternalField();
        }
    }
}

template<class Type>
struct pointConstraintsConstrainCornersFunctor
{
    __host__ __device__
    Type operator()(const tensor& t, const Type& pf)
    {
        return transform(t,pf);
    }
};

template<class Type>
void pointConstraints::constrainCorners
(
    GeometricField<Type, pointPatchField, pointMesh>& pf
) const
{
    thrust::transform
    (
        gpuPatchPatchPointConstraintTensors_.begin(),
        gpuPatchPatchPointConstraintTensors_.end(),
        thrust::make_permutation_iterator
        (
            pf.getField().begin(),
            gpuPatchPatchPointConstraintPoints_.begin()
        ),
        thrust::make_permutation_iterator
        (
            pf.getField().begin(),
            gpuPatchPatchPointConstraintPoints_.begin()
        ),
        pointConstraintsConstrainCornersFunctor<Type>()
    );
}


template<class Type>
void pointConstraints::constrain
(
    GeometricField<Type, pointPatchField, pointMesh>& pf,
    const bool overrideFixedValue
) const
{
    // Override constrained pointPatchField types with the constraint value.
    // This relies on only constrained pointPatchField implementing the evaluate
    // function
    pf.correctBoundaryConditions();

    // Sync any dangling points
    syncUntransformedData(mesh()(), pf.internalField(), maxMagSqrEqOp<Type>());

    // Apply multiple constraints on edge/corner points
    constrainCorners(pf);

    if (overrideFixedValue)
    {
        setPatchFields(pf);
    }
}


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace Foam

// ************************************************************************* //
