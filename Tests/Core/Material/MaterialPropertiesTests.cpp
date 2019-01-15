// This file is part of the Acts project.
//
// Copyright (C) 2017-2018 Acts project team
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

///  Boost include(s)
#define BOOST_TEST_MODULE MaterialProperties Tests
#include <boost/test/included/unit_test.hpp>
#include <climits>
#include "Acts/Material/Material.hpp"
#include "Acts/Material/MaterialProperties.hpp"

namespace Acts {

namespace Test {

  /// Test the constructors
  BOOST_AUTO_TEST_CASE(MaterialProperties_construction_test)
  {
    // constructor only from arguments
    MaterialProperties a(1., 2., 3., 4., 5., 6.);
    /// constructor with material
    MaterialProperties b(Material(1., 2., 3., 4., 5.), 6.);

    // The thickness should be 6
    BOOST_CHECK_CLOSE(a.thickness(), 6., 0.0001);
    BOOST_CHECK_CLOSE(a.thicknessInX0(), 6., 0.0001);
    BOOST_CHECK_CLOSE(a.thicknessInL0(), 3., 0.0001);
    BOOST_CHECK_EQUAL(a.averageA(), 3.);
    BOOST_CHECK_EQUAL(a.averageZ(), 4.);
    BOOST_CHECK_EQUAL(a.averageRho(), 5.);
    BOOST_CHECK_CLOSE(a.zOverAtimesRho(), 6.666666666, 0.0001);

    /// Check if they are equal
    BOOST_CHECK_EQUAL(a, b);

    /// Check the move construction
    MaterialProperties bMoved(std::move(b));
    /// Check if they are equal
    BOOST_CHECK_EQUAL(a, bMoved);

    /// Check the move assignment
    MaterialProperties bMovedAssigned = std::move(bMoved);
    /// Check if they are equal
    BOOST_CHECK_EQUAL(a, bMovedAssigned);
  }

  /// Test the constructors
  BOOST_AUTO_TEST_CASE(MaterialProperties_compound_test)
  {
    MaterialProperties a(1., 2., 3., 4., 5., 1.);
    MaterialProperties b(2., 4., 6., 8., 10., 2.);
    MaterialProperties c(4., 8., 12., 16., 20., 3.);

    std::vector<MaterialProperties> compound = {{a, b, c}};

    /// Thickness is scaled to unit here
    MaterialProperties abc(compound, true);

    // Unit legnth thickness
    BOOST_CHECK_CLOSE(abc.thickness(), 1., 0.0001);

    // Thickness in X0 is additive
    BOOST_CHECK_CLOSE(abc.thicknessInX0(),
                      a.thicknessInX0() + b.thicknessInX0() + c.thicknessInX0(),
                      0.0001);

    BOOST_CHECK_CLOSE(
        abc.thickness() / abc.averageX0(), abc.thicknessInX0(), 0.0001);

    BOOST_CHECK_CLOSE(abc.thicknessInL0(),
                      a.thicknessInL0() + b.thicknessInL0() + c.thicknessInL0(),
                      0.0001);

    // Thinkness is NOT unit scaled here
    MaterialProperties abcNS(compound, false);

    // The density scales with the thickness then
    BOOST_CHECK_CLOSE(abcNS.averageRho(),
                      (a.thickness() * a.averageRho()
                       + b.thickness() * b.averageRho()
                       + c.thickness() * c.averageRho())
                          / (a.thickness() + b.thickness() + c.thickness()),
                      0.0001);

    // The material properties are not the same
    BOOST_TEST(abc != abcNS);
    // Because thickness is not the same
    BOOST_TEST(abc.thickness() != abcNS.thickness());
    // And the densities are differnt
    BOOST_TEST(abc.averageRho() != abcNS.averageRho());
    // Though the amount should be the same
    BOOST_CHECK_CLOSE(abc.thicknessInX0(), abcNS.thicknessInX0(), 0.0001);
    BOOST_CHECK_CLOSE(abc.thicknessInL0(), abcNS.thicknessInL0(), 0.0001);
    BOOST_CHECK_CLOSE(abc.averageA(), abcNS.averageA(), 0.0001);
    BOOST_CHECK_CLOSE(abc.averageZ(), abcNS.averageZ(), 0.0001);
    BOOST_CHECK_CLOSE(abc.averageRho() * abc.thickness(),
                      abcNS.averageRho() * abcNS.thickness(),
                      0.0001);
  }

  // Test the Scaling
  BOOST_AUTO_TEST_CASE(MaterialProperties_scale_test)
  {
    // construct the material properties from arguments
    MaterialProperties mat(1., 2., 3., 4., 5., 0.1);
    MaterialProperties halfMat(1., 2., 3., 4., 5., 0.05);
    MaterialProperties halfScaled = mat;
    halfScaled *= 0.5;

    BOOST_TEST(mat != halfMat);
    BOOST_TEST(halfMat == halfScaled);

    // this means half the scattering
    BOOST_CHECK_CLOSE(
        mat.thicknessInX0(), 2. * halfMat.thicknessInX0(), 0.0001);
    BOOST_CHECK_CLOSE(
        mat.thicknessInL0(), 2. * halfMat.thicknessInL0(), 0.0001);

    // and half the energy loss, given
    BOOST_CHECK_CLOSE(mat.thickness() * mat.averageRho(),
                      2. * halfMat.thickness() * halfMat.averageRho(),
                      0.0001);
  }

}  // namespace Test
}  // namespace Acts