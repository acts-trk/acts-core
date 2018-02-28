// This file is part of the ACTS project.
//
// Copyright (C) 2017-2018 ACTS project team
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "ACTS/Plugins/DD4hepPlugins/DD4hepLayerBuilder.hpp"
#include "ACTS/Layers/CylinderLayer.hpp"
#include "ACTS/Layers/DiscLayer.hpp"
#include "ACTS/Layers/GenericApproachDescriptor.hpp"
#include "ACTS/Layers/Layer.hpp"
#include "ACTS/Layers/ProtoLayer.hpp"
#include "ACTS/Material/SurfaceMaterialProxy.hpp"
#include "ACTS/Plugins/DD4hepPlugins/DD4hepDetElement.hpp"
#include "ACTS/Plugins/DD4hepPlugins/IActsExtension.hpp"
#include "ACTS/Surfaces/CylinderSurface.hpp"
#include "ACTS/Surfaces/RadialBounds.hpp"
#include "ACTS/Surfaces/Surface.hpp"
#include "ACTS/Surfaces/SurfaceArray.hpp"
#include "ACTS/Tools/LayerCreator.hpp"
#include "ACTS/Utilities/BinUtility.hpp"
#include "ACTS/Utilities/BinnedArrayXD.hpp"
#include "ACTS/Utilities/Units.hpp"
#include "DD4hep/Detector.h"
#include "TGeoManager.h"
#include "TGeoMatrix.h"

#include <boost/algorithm/string.hpp>
#include "ACTS/Material/HomogeneousSurfaceMaterial.hpp"

Acts::DD4hepLayerBuilder::DD4hepLayerBuilder(
    const Acts::DD4hepLayerBuilder::Config& config,
    std::unique_ptr<const Logger>           logger)
  : m_cfg(), m_logger(std::move(logger))
{
  setConfiguration(config);
}

Acts::DD4hepLayerBuilder::~DD4hepLayerBuilder()
{
}

void
Acts::DD4hepLayerBuilder::setConfiguration(
    const Acts::DD4hepLayerBuilder::Config& config)
{
  m_cfg = config;
}

const Acts::LayerVector
Acts::DD4hepLayerBuilder::negativeLayers() const
{
  LayerVector layers;
  if (m_cfg.negativeLayers.empty()) {
    ACTS_VERBOSE("[L] No layers handed over for negative volume.");
  } else {
    ACTS_VERBOSE("[L] Received layers for negative volume -> creating "
                 "disc layers");
    // go through layers
    for (auto& detElement : m_cfg.negativeLayers) {
      // prepare the layer surfaces
      std::vector<const Surface*> layerSurfaces;
      // access the extension of the layer
      // at this stage all layer detElements have extension (checked in
      // ConvertDD4hepDetector)
      Acts::IActsExtension* detExtension
          = detElement.extension<Acts::IActsExtension>();
      // access the axis orienation of the modules
      std::string axes = detExtension->axes();
      // collect the sensitive detector elements possibly contained by the layer
      collectSensitive(detElement, layerSurfaces, axes);
      // access the global transformation matrix of the layer
      auto transform
          = convertTransform(&(detElement.nominal().worldTransformation()));
      // get the shape of the layer
      TGeoShape* geoShape
          = detElement.placement().ptr()->GetVolume()->GetShape();
      // create the proto layer
      ProtoLayer pl(layerSurfaces);
      if (detExtension->buildEnvelope()) {
        // set the values of the proto layer in case enevelopes are handed over
        pl.envR = {detExtension->envelopeR(), detExtension->envelopeR()};
        pl.envZ = {detExtension->envelopeZ(), detExtension->envelopeZ()};
      } else if (geoShape) {
        TGeoTubeSeg* tube = dynamic_cast<TGeoTubeSeg*>(geoShape);
        if (!tube)
          ACTS_ERROR(
              "[L] Disc layer has wrong shape - needs to be TGeoTubeSeg!");
        // extract the boundaries
        double rMin = tube->GetRmin() * units::_cm;
        double rMax = tube->GetRmax() * units::_cm;
        double zMin
            = (transform->translation()
               - transform->rotation().col(2) * tube->GetDz() * units::_cm)
                  .z();
        double zMax
            = (transform->translation()
               + transform->rotation().col(2) * tube->GetDz() * units::_cm)
                  .z();
        if (zMin > zMax) std::swap(zMin, zMax);

        // check if layer has surfaces
        if (layerSurfaces.empty()) {
          // create layer without surfaces
          // manually create a proto layer
          pl.minR = rMin;
          pl.maxR = rMax;
          pl.minZ = zMin;
          pl.maxZ = zMax;
          pl.envR = {0., 0.};
          pl.envZ = {0., 0.};
        } else {
          // set the values of the proto layer in case dimensions are given by
          // geometry
          pl.envZ = {std::abs(zMin - pl.minZ), std::abs(zMax - pl.maxZ)};
          pl.envR = {std::abs(rMin - pl.minR), std::abs(rMax - pl.maxR)};
        }
      } else
        throw std::logic_error(
            std::string("Layer DetElement: ") + detElement.name()
            + std::string(" has neither a shape nor tolerances for envelopes "
                          "added to it¥s extension. Please check your detector "
                          "constructor!"));

      // if the layer should carry material it will be marked by assigning a
      // SurfaceMaterialProxy
      std::shared_ptr<const SurfaceMaterialProxy> materialProxy(nullptr);
      // the approachdescriptor telling where the material sits on the layer
      // (inner, middle, outer) Surface
      std::unique_ptr<Acts::ApproachDescriptor> approachDescriptor = nullptr;
      // material position on the layer canbe inner, outer or center and will
      // be accessed from the ActsExtensions
      Acts::LayerMaterialPos layerPos = LayerMaterialPos::inner;
      // check if layer should have material
      if (detExtension->hasSupportMaterial()) {
        std::pair<size_t, size_t> materialBins = detExtension->materialBins();
        size_t           bins1 = materialBins.first;
        size_t           bins2 = materialBins.second;
        Acts::BinUtility materialBinUtil(
            bins1, -M_PI, M_PI, Acts::closed, Acts::binPhi);
        materialBinUtil += Acts::BinUtility(
            bins2, pl.minR, pl.maxR, Acts::open, Acts::binR, transform);
        // and create material proxy to mark layer for material mapping
        materialProxy
            = std::make_shared<const SurfaceMaterialProxy>(materialBinUtil);
        // access the material position
        layerPos = detExtension->layerMaterialPosition();
        ACTS_VERBOSE(
            "[L] Layer is marked to carry support material on Surface ( "
            "inner=0 / center=1 / outer=2 ) :   "
            << layerPos
            << "    with binning: ["
            << bins1
            << ", "
            << bins2
            << "]");
        // Create an approachdescriptor for the layer
        // create the new surfaces for the approachdescriptor
        std::vector<const Acts::Surface*> aSurfaces;
        // The layer thicknesses
        auto layerThickness
            = std::fabs(pl.minZ - pl.maxZ) + pl.envZ.first + pl.envZ.second;
        // create the inner and outer boundary surfaces
        // first create the positions
        Vector3D innerPos = transform->translation()
            - transform->rotation().col(2) * layerThickness * 0.5;
        Vector3D outerPos = transform->translation()
            + transform->rotation().col(2) * layerThickness * 0.5;

        if (innerPos.z() > outerPos.z()) std::swap(innerPos, outerPos);

        Acts::DiscSurface* innerBoundary
            = new Acts::DiscSurface(std::make_shared<const Transform3D>(
                                        transform->rotation(), innerPos),
                                    pl.minR,
                                    pl.maxR);

        Acts::DiscSurface* outerBoundary
            = new Acts::DiscSurface(std::make_shared<const Transform3D>(
                                        transform->rotation(), outerPos),
                                    pl.minR,
                                    pl.maxR);

        Acts::DiscSurface* centralSurface
            = new Acts::DiscSurface(transform, pl.minR, pl.maxR);

        // set material surface
        if (layerPos == Acts::LayerMaterialPos::inner)
          innerBoundary->setAssociatedMaterial(materialProxy);

        if (layerPos == Acts::LayerMaterialPos::outer)
          outerBoundary->setAssociatedMaterial(materialProxy);

        if (layerPos == Acts::LayerMaterialPos::central)
          centralSurface->setAssociatedMaterial(materialProxy);

        // collect approach surfaces
        aSurfaces.push_back(innerBoundary);
        aSurfaces.push_back(outerBoundary);
        aSurfaces.push_back(centralSurface);
        // create an ApproachDescriptor with standard surfaces - these
        // will be deleted by the approach descriptor
        approachDescriptor
            = std::make_unique<Acts::GenericApproachDescriptor<Acts::Surface>>(
                aSurfaces);
      }
      std::shared_ptr<Layer> negativeLayer = nullptr;
      // In case the layer is sensitive
      if (detElement.volume().isSensitive()) {
        // Create the sensitive surface
        auto sensitiveSurf = createSensitiveSurface(detElement, true);
        // Create the surfaceArray

        Acts::SurfaceArray::SingleElementLookup gl(sensitiveSurf);
        std::unique_ptr<Acts::SurfaceArray>     sArray
            = std::make_unique<SurfaceArray>(
                gl, std::vector<const Surface*>({sensitiveSurf}));

        // create the share disc bounds
        auto   dBounds = std::make_shared<const RadialBounds>(pl.minR, pl.maxR);
        double thickness = std::fabs(pl.maxZ - pl.minZ);
        // Create the layer containing the sensitive surface
        negativeLayer = DiscLayer::create(transform,
                                          dBounds,
                                          std::move(sArray),
                                          thickness,
                                          std::move(approachDescriptor),
                                          Acts::active);

      } else {
        negativeLayer
            = m_cfg.layerCreator->discLayer(layerSurfaces,
                                            m_cfg.bTypeR,
                                            m_cfg.bTypePhi,
                                            pl,
                                            transform,
                                            std::move(approachDescriptor));
      }

      // get the possible material if no surfaces are handed over
      std::shared_ptr<const HomogeneousSurfaceMaterial> surfMaterial = nullptr;

      dd4hep::Material ddmaterial = detElement.volume().material();
      if (!boost::iequals(ddmaterial.name(), "vacuum")) {
        Material layerMaterial(ddmaterial.radLength() * Acts::units::_cm,
                               ddmaterial.intLength() * Acts::units::_cm,
                               ddmaterial.A(),
                               ddmaterial.Z(),
                               ddmaterial.density() / pow(Acts::units::_cm, 3));

        MaterialProperties materialProperties(layerMaterial,
                                              fabs(pl.maxR - pl.minR));

        surfMaterial = std::make_shared<const HomogeneousSurfaceMaterial>(
            materialProperties);
      }

      negativeLayer->surfaceRepresentation().setAssociatedMaterial(
          surfMaterial);
      // push back created layer
      layers.push_back(negativeLayer);
    }
  }

  return layers;
}

const Acts::LayerVector
Acts::DD4hepLayerBuilder::centralLayers() const
{
  LayerVector layers;
  if (m_cfg.centralLayers.empty()) {
    ACTS_VERBOSE("[L] No layers handed over for central volume!");
  } else {
    ACTS_VERBOSE("[L] Received layers for central volume -> creating "
                 "cylindrical layers");
    // go through layers
    for (auto& detElement : m_cfg.centralLayers) {
      // prepare the layer surfaces
      std::vector<const Surface*> layerSurfaces;
      // access the extension of the layer
      // at this stage all layer detElements have extension (checked in
      // ConvertDD4hepDetector)
      Acts::IActsExtension* detExtension
          = detElement.extension<Acts::IActsExtension>();
      // access the axis orienation of the modules
      std::string axes = detExtension->axes();
      // collect the sensitive detector elements possibly contained by the layer
      collectSensitive(detElement, layerSurfaces, axes);
      // access the global transformation matrix of the layer
      auto transform
          = convertTransform(&(detElement.nominal().worldTransformation()));
      // get the shape of the layer
      TGeoShape* geoShape
          = detElement.placement().ptr()->GetVolume()->GetShape();
      // create the proto layer
      ProtoLayer pl(layerSurfaces);
      if (detExtension->buildEnvelope()) {
        // set the values of the proto layer in case enevelopes are handed over
        pl.envR = {detExtension->envelopeR(), detExtension->envelopeR()};
        pl.envZ = {detExtension->envelopeZ(), detExtension->envelopeZ()};
      } else if (geoShape) {
        TGeoTubeSeg* tube = dynamic_cast<TGeoTubeSeg*>(geoShape);
        if (!tube)
          ACTS_ERROR(
              "[L] Cylinder layer has wrong shape - needs to be TGeoTubeSeg!");

        // extract the boundaries
        double rMin = tube->GetRmin() * units::_cm;
        double rMax = tube->GetRmax() * units::_cm;
        double dz   = tube->GetDz() * units::_cm;
        // check if layer has surfaces
        if (layerSurfaces.empty()) {
          // create layer without surfaces
          // manually create a proto layer
          pl.minR = rMin;
          pl.maxR = rMax;
          pl.minZ = -dz;
          pl.maxZ = dz;
          pl.envR = {0., 0.};
          pl.envZ = {0., 0.};
        } else {
          // set the values of the proto layer in case dimensions are given by
          // geometry
          pl.envZ = {std::abs(-dz - pl.minZ), std::abs(dz - pl.maxZ)};
          pl.envR = {std::abs(rMin - pl.minR), std::abs(rMax - pl.maxR)};
        }
      } else
        throw std::logic_error(
            std::string("Layer DetElement: ") + detElement.name()
            + std::string(" has neither a shape nor tolerances for envelopes "
                          "added to it¥s extension. Please check your detector "
                          "constructor!"));

      double halfZ = (pl.minZ - pl.maxZ) * 0.5;

      // if the layer should carry material it will be marked by assigning a
      // SurfaceMaterialProxy
      std::shared_ptr<const SurfaceMaterialProxy> materialProxy(nullptr);
      // the approachdescriptor telling where the material sits on the layer
      // (inner, middle, outer) Surface
      std::unique_ptr<Acts::ApproachDescriptor> approachDescriptor = nullptr;
      // material position on the layer can be inner, outer or center and will
      // be accessed from the ActsExtensions
      Acts::LayerMaterialPos layerPos = LayerMaterialPos::inner;

      // check if layer should have material
      if (detExtension->hasSupportMaterial()) {

        // Create an approachdescriptor for the layer
        // create the new surfaces for the approachdescriptor
        std::vector<const Acts::Surface*> aSurfaces;
        // create the inner boundary surface
        Acts::CylinderSurface* innerBoundary
            = new Acts::CylinderSurface(transform, pl.minR, halfZ);
        // create outer boundary surface
        Acts::CylinderSurface* outerBoundary
            = new Acts::CylinderSurface(transform, pl.maxR, halfZ);
        // create the central surface
        Acts::CylinderSurface* centralSurface = new Acts::CylinderSurface(
            transform, (pl.minR + pl.maxR) * 0.5, halfZ);

        std::pair<size_t, size_t> materialBins = detExtension->materialBins();

        size_t           bins1 = materialBins.first;
        size_t           bins2 = materialBins.second;
        Acts::BinUtility materialBinUtil(
            bins1, -M_PI, M_PI, Acts::closed, Acts::binPhi);
        materialBinUtil += Acts::BinUtility(
            bins2, -halfZ, halfZ, Acts::open, Acts::binZ, transform);
        // and create material proxy to mark layer for material mapping
        materialProxy
            = std::make_shared<const SurfaceMaterialProxy>(materialBinUtil);
        // access the material position
        layerPos = detExtension->layerMaterialPosition();
        ACTS_VERBOSE(
            "[L] Layer is marked to carry support material on Surface ( "
            "inner=0 / center=1 / outer=2 ) :   "
            << layerPos
            << "    with binning: ["
            << bins1
            << ", "
            << bins2
            << "]");

        // check if the material should be set to the inner or outer boundary
        // and set it in case
        if (layerPos == Acts::LayerMaterialPos::inner)
          innerBoundary->setAssociatedMaterial(materialProxy);

        if (layerPos == Acts::LayerMaterialPos::outer)
          outerBoundary->setAssociatedMaterial(materialProxy);

        if (layerPos == Acts::LayerMaterialPos::central)
          centralSurface->setAssociatedMaterial(materialProxy);

        // collect the surfaces
        aSurfaces.push_back(innerBoundary);
        aSurfaces.push_back(centralSurface);
        aSurfaces.push_back(outerBoundary);

        // create an ApproachDescriptor with standard surfaces - these
        // will be deleted by the approach descriptor
        approachDescriptor
            = std::make_unique<Acts::GenericApproachDescriptor<Acts::Surface>>(
                aSurfaces);
      }

      std::shared_ptr<Layer> centralLayer = nullptr;
      // In case the layer is sensitive
      if (detElement.volume().isSensitive()) {
        // Create the sensitive surface
        auto sensitiveSurf = createSensitiveSurface(detElement);
        // Create the surfaceArray
        Acts::SurfaceArray::SingleElementLookup gl(sensitiveSurf);
        std::unique_ptr<Acts::SurfaceArray>     sArray
            = std::make_unique<SurfaceArray>(
                gl, std::vector<const Surface*>({sensitiveSurf}));

        // create the layer
        double layerR    = (pl.minR + pl.maxR) * 0.5;
        double thickness = std::fabs(pl.maxR - pl.minR);
        std::shared_ptr<const CylinderBounds> cBounds(
            new CylinderBounds(layerR, halfZ));
        // Create the layer containing the sensitive surface
        centralLayer = CylinderLayer::create(transform,
                                             cBounds,
                                             std::move(sArray),
                                             thickness,
                                             std::move(approachDescriptor),
                                             Acts::active);

      } else {
        centralLayer
            = m_cfg.layerCreator->cylinderLayer(layerSurfaces,
                                                m_cfg.bTypePhi,
                                                m_cfg.bTypeZ,
                                                pl,
                                                transform,
                                                std::move(approachDescriptor));
      }

      // get the possible material if no surfaces are handed over
      std::shared_ptr<const HomogeneousSurfaceMaterial> surfMaterial = nullptr;

      dd4hep::Material ddmaterial = detElement.volume().material();
      if (!boost::iequals(ddmaterial.name(), "vacuum")) {
        Material layerMaterial(ddmaterial.radLength() * Acts::units::_cm,
                               ddmaterial.intLength() * Acts::units::_cm,
                               ddmaterial.A(),
                               ddmaterial.Z(),
                               ddmaterial.density() / pow(Acts::units::_cm, 3));

        MaterialProperties materialProperties(layerMaterial,
                                              fabs(pl.maxR - pl.minR));

        surfMaterial = std::make_shared<const HomogeneousSurfaceMaterial>(
            materialProperties);

        //   innerBoundary->setAssociatedMaterial(surfMaterial);
      }

      centralLayer->surfaceRepresentation().setAssociatedMaterial(surfMaterial);

      // push back created layer
      layers.push_back(centralLayer);
    }
  }
  return layers;
}

const Acts::LayerVector
Acts::DD4hepLayerBuilder::positiveLayers() const
{
  LayerVector layers;
  if (m_cfg.positiveLayers.empty()) {
    ACTS_VERBOSE("[L] No layers handed over for positive volume!");
  } else {
    ACTS_VERBOSE("[L] Received layers for positive volume -> creating "
                 "disc layers");
    // go through layers
    for (auto& detElement : m_cfg.positiveLayers) {
      // prepare the layer surfaces
      std::vector<const Surface*> layerSurfaces;
      // access the extension of the layer
      // at this stage all layer detElements have extension (checked in
      // ConvertDD4hepDetector)
      Acts::IActsExtension* detExtension
          = detElement.extension<Acts::IActsExtension>();
      // access the axis orienation of the modules
      std::string axes = detExtension->axes();
      // collect the sensitive detector elements possibly contained by the layer
      collectSensitive(detElement, layerSurfaces, axes);
      // access the global transformation matrix of the layer
      auto transform
          = convertTransform(&(detElement.nominal().worldTransformation()));
      // get the shape of the layer
      TGeoShape* geoShape
          = detElement.placement().ptr()->GetVolume()->GetShape();
      // create the proto layer
      ProtoLayer pl(layerSurfaces);
      if (detExtension->buildEnvelope()) {
        // set the values of the proto layer in case enevelopes are handed over
        pl.envR = {detExtension->envelopeR(), detExtension->envelopeR()};
        pl.envZ = {detExtension->envelopeZ(), detExtension->envelopeZ()};
      } else if (geoShape) {
        TGeoTubeSeg* tube = dynamic_cast<TGeoTubeSeg*>(geoShape);
        if (!tube)
          ACTS_ERROR(
              "[L] Disc layer has wrong shape - needs to be TGeoTubeSeg!");
        // extract the boundaries
        double rMin = tube->GetRmin() * units::_cm;
        double rMax = tube->GetRmax() * units::_cm;
        double zMin
            = (transform->translation()
               - transform->rotation().col(2) * tube->GetDz() * units::_cm)
                  .z();
        double zMax
            = (transform->translation()
               + transform->rotation().col(2) * tube->GetDz() * units::_cm)
                  .z();
        if (zMin > zMax) std::swap(zMin, zMax);

        // check if layer has surfaces
        if (layerSurfaces.empty()) {
          // create layer without surfaces
          pl.minR = rMin;
          pl.maxR = rMax;
          pl.minZ = zMin;
          pl.maxZ = zMax;
          pl.envR = {0., 0.};
          pl.envZ = {0., 0.};
        } else {
          // set the values of the proto layer in case dimensions are given by
          // geometry
          pl.envZ = {std::abs(zMin - pl.minZ), std::abs(zMax - pl.maxZ)};
          pl.envR = {std::abs(rMin - pl.minR), std::abs(rMax - pl.maxR)};
        }
      } else
        throw std::logic_error(
            std::string("Layer DetElement: ") + detElement.name()
            + std::string(" has neither a shape nor tolerances for envelopes "
                          "added to it¥s extension. Please check your detector "
                          "constructor!"));

      // if the layer should carry material it will be marked by assigning a
      // SurfaceMaterialProxy
      std::shared_ptr<const SurfaceMaterialProxy> materialProxy(nullptr);
      // the approachdescriptor telling where the material sits on the layer
      // (inner, middle, outer) Surface
      std::unique_ptr<Acts::ApproachDescriptor> approachDescriptor = nullptr;
      // material position on the layer can be inner, outer or center and will
      // be accessed from the ActsExtensions
      Acts::LayerMaterialPos layerPos = LayerMaterialPos::inner;
      // check if layer should have material
      if (detExtension->hasSupportMaterial()) {
        std::pair<size_t, size_t> materialBins = detExtension->materialBins();
        size_t           bins1 = materialBins.first;
        size_t           bins2 = materialBins.second;
        Acts::BinUtility materialBinUtil(
            bins1, -M_PI, M_PI, Acts::closed, Acts::binPhi);
        materialBinUtil += Acts::BinUtility(
            bins2, pl.minR, pl.maxR, Acts::open, Acts::binR, transform);
        // and create material proxy to mark layer for material mapping
        materialProxy
            = std::make_shared<const SurfaceMaterialProxy>(materialBinUtil);
        // access the material position
        layerPos = detExtension->layerMaterialPosition();
        ACTS_VERBOSE(
            "[L] Layer is marked to carry support material on Surface ( "
            "inner=0 / center=1 / outer=2 ) :   "
            << layerPos
            << "    with binning: ["
            << bins1
            << ", "
            << bins2
            << "]");
        // Create an approachdescriptor for the layer
        // create the new surfaces for the approachdescriptor
        std::vector<const Acts::Surface*> aSurfaces;
        // The layer thicknesses
        auto layerThickness
            = std::fabs(pl.minZ - pl.maxZ) + pl.envZ.first + pl.envZ.second;
        // create the inner and outer boundary surfaces
        // first create the positions
        Vector3D innerPos = transform->translation()
            - transform->rotation().col(2) * layerThickness * 0.5;
        Vector3D outerPos = transform->translation()
            + transform->rotation().col(2) * layerThickness * 0.5;

        if (innerPos.z() > outerPos.z()) std::swap(innerPos, outerPos);

        Acts::DiscSurface* innerBoundary
            = new Acts::DiscSurface(std::make_shared<const Transform3D>(
                                        transform->rotation(), innerPos),
                                    pl.minR,
                                    pl.maxR);

        Acts::DiscSurface* outerBoundary
            = new Acts::DiscSurface(std::make_shared<const Transform3D>(
                                        transform->rotation(), outerPos),
                                    pl.minR,
                                    pl.maxR);

        Acts::DiscSurface* centralSurface
            = new Acts::DiscSurface(transform, pl.minR, pl.maxR);

        // set material surface
        if (layerPos == Acts::LayerMaterialPos::inner)
          innerBoundary->setAssociatedMaterial(materialProxy);

        if (layerPos == Acts::LayerMaterialPos::outer)
          outerBoundary->setAssociatedMaterial(materialProxy);

        if (layerPos == Acts::LayerMaterialPos::central)
          centralSurface->setAssociatedMaterial(materialProxy);
        // collect approach surfaces
        aSurfaces.push_back(innerBoundary);
        aSurfaces.push_back(centralSurface);
        aSurfaces.push_back(outerBoundary);
        // create an ApproachDescriptor with standard surfaces - these
        // will be deleted by the approach descriptor
        approachDescriptor
            = std::make_unique<Acts::GenericApproachDescriptor<Acts::Surface>>(
                aSurfaces);
      }

      std::shared_ptr<Layer> positiveLayer = nullptr;
      // In case the layer is sensitive
      if (detElement.volume().isSensitive()) {
        // Create the sensitive surface
        auto sensitiveSurf = createSensitiveSurface(detElement, true);
        // Create the surfaceArray
        Acts::SurfaceArray::SingleElementLookup gl(sensitiveSurf);
        std::unique_ptr<Acts::SurfaceArray>     sArray
            = std::make_unique<SurfaceArray>(
                gl, std::vector<const Surface*>({sensitiveSurf}));

        // create the share disc bounds
        auto   dBounds = std::make_shared<const RadialBounds>(pl.minR, pl.maxR);
        double thickness = std::fabs(pl.maxZ - pl.minZ);
        // Create the layer containing the sensitive surface
        positiveLayer = DiscLayer::create(transform,
                                          dBounds,
                                          std::move(sArray),
                                          thickness,
                                          std::move(approachDescriptor),
                                          Acts::active);

      } else {
        positiveLayer
            = m_cfg.layerCreator->discLayer(layerSurfaces,
                                            m_cfg.bTypeR,
                                            m_cfg.bTypePhi,
                                            pl,
                                            transform,
                                            std::move(approachDescriptor));
      }

      // get the possible material if no surfaces are handed over
      std::shared_ptr<const HomogeneousSurfaceMaterial> surfMaterial = nullptr;

      dd4hep::Material ddmaterial = detElement.volume().material();
      if (!boost::iequals(ddmaterial.name(), "vacuum")) {
        Material layerMaterial(ddmaterial.radLength() * Acts::units::_cm,
                               ddmaterial.intLength() * Acts::units::_cm,
                               ddmaterial.A(),
                               ddmaterial.Z(),
                               ddmaterial.density() / pow(Acts::units::_cm, 3));

        MaterialProperties materialProperties(layerMaterial,
                                              fabs(pl.maxR - pl.minR));

        surfMaterial = std::make_shared<const HomogeneousSurfaceMaterial>(
            materialProperties);
      }
      positiveLayer->surfaceRepresentation().setAssociatedMaterial(
          surfMaterial);

      // push back created layer
      layers.push_back(positiveLayer);
    }
  }
  return layers;
}

void
Acts::DD4hepLayerBuilder::collectSensitive(
    const dd4hep::DetElement&          detElement,
    std::vector<const Acts::Surface*>& surfaces,
    const std::string&                 axes) const
{
  const dd4hep::DetElement::Children& children = detElement.children();
  if (!children.empty()) {
    for (auto& child : children) {
      dd4hep::DetElement childDetElement = child.second;
      if (childDetElement.volume().isSensitive()) {
        // create the surface
        surfaces.push_back(
            createSensitiveSurface(childDetElement, false, axes));
      }
      collectSensitive(childDetElement, surfaces, axes);
    }
  }
}
const Acts::Surface*
Acts::DD4hepLayerBuilder::createSensitiveSurface(
    const dd4hep::DetElement& detElement,
    bool                      isDisc,
    const std::string&        axes) const
{
  // access the possible material
  std::shared_ptr<const Acts::SurfaceMaterial> material = nullptr;
  // access the possibly shared DigitizationModule
  std::shared_ptr<const DigitizationModule> digiModule = nullptr;
  // access the possible extension of the DetElement
  Acts::IActsExtension* detExtension = nullptr;
  try {
    detExtension = detElement.extension<Acts::IActsExtension>();
  } catch (std::runtime_error& e) {
  }
  if (detExtension) {
    material   = detExtension->material();
    digiModule = detExtension->digitizationModule();
  }

  // create the corresponding detector element
  Acts::DD4hepDetElement* dd4hepDetElement
      = new Acts::DD4hepDetElement(detElement,
                                   axes,
                                   units::_cm,
                                   isDisc,
                                   material,
                                   m_cfg.buildDigitizationModules,
                                   digiModule);
  // return the surface
  return (&(dd4hepDetElement->surface()));
}

std::shared_ptr<const Acts::Transform3D>
Acts::DD4hepLayerBuilder::convertTransform(const TGeoMatrix* tGeoTrans) const
{
  // get the placement and orientation in respect to its mother
  const Double_t* rotation    = tGeoTrans->GetRotationMatrix();
  const Double_t* translation = tGeoTrans->GetTranslation();
  auto            transform   = std::make_shared<const Transform3D>(
      Acts::Vector3D(rotation[0], rotation[3], rotation[6]),
      Acts::Vector3D(rotation[1], rotation[4], rotation[7]),
      Acts::Vector3D(rotation[2], rotation[5], rotation[8]),
      Acts::Vector3D(translation[0] * units::_cm,
                     translation[1] * units::_cm,
                     translation[2] * units::_cm));
  return (transform);
}