// SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

// gtest has to come before pbrt stuff
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "GeometryInstancePrinter.h"
#include "Matchers.h"
#include "MockGeometryLoader.h"
#include "MockMeshLoader.h"

#include <DemandPbrtScene/GeometryCache.h>
#include <DemandPbrtScene/MaterialAdapters.h>
#include <DemandPbrtScene/Options.h>
#include <DemandPbrtScene/Params.h>
#include <DemandPbrtScene/SceneProxy.h>

#include <OptiXToolkit/DemandGeometry/Mocks/Matchers.h>
#include <OptiXToolkit/DemandGeometry/Mocks/OptixCompare.h>
#include <OptiXToolkit/Error/cuErrorCheck.h>
#include <OptiXToolkit/Error/cudaErrorCheck.h>
#include <OptiXToolkit/Memory/BitCast.h>
#include <OptiXToolkit/Memory/SyncVector.h>
#include <OptiXToolkit/PbrtSceneLoader/MeshReader.h>
#include <OptiXToolkit/PbrtSceneLoader/SceneDescription.h>

#include <cuda.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>

using namespace demandPbrtScene;
using namespace otk::pbrt;
using namespace demandPbrtScene::testing;
using namespace otk::testing;
using namespace ::testing;

using P2 = pbrt::Point2f;
using P3 = pbrt::Point3f;
using B3 = pbrt::Bounds3f;

constexpr const char* DIFFUSE_MAP_FILENAME{ "diffuse.png" };
constexpr const char* ALPHA_MAP_FILENAME{ "alpha.png" };
constexpr uint_t      ARBITRARY_PRIMITIVE_GROUP_END{ 222U };

template <typename Thing>
B3 transformBounds( const Thing& thing )
{
    return thing.transform( thing.bounds );
}

static MaterialGroup materialGroupFromPlasticMaterial( const PlasticMaterial& value, uint_t primitiveIndex )
{
    MaterialGroup result{};
    const auto    toFloat3    = []( const P3& pt ) { return make_float3( pt.x, pt.y, pt.z ); };
    result.material.Ka        = toFloat3( value.Ka );
    result.material.Kd        = toFloat3( value.Kd );
    result.material.Ks        = toFloat3( value.Ks );
    result.alphaMapFileName   = value.alphaMapFileName;
    result.diffuseMapFileName = value.diffuseMapFileName;
    result.material.flags     = plasticMaterialFlags( value );
    result.primitiveIndexEnd  = primitiveIndex;
    return result;
}

static PlasticMaterial expectedMaterial()
{
    PlasticMaterial material{};
    material.Ka = P3{ 0.1f, 0.2f, 0.3f };
    material.Kd = P3{ 0.4f, 0.5f, 0.6f };
    material.Ks = P3{ 0.7f, 0.8f, 0.9f };
    return material;
}

static PlasticMaterial expectedSecondMaterial()
{
    PlasticMaterial material{};
    material.Ka = P3{ 0.7f, 0.8f, 0.9f };
    material.Kd = P3{ 0.4f, 0.5f, 0.6f };
    material.Ks = P3{ 0.1f, 0.2f, 0.3f };
    return material;
}

static ShapeDefinition translatedTriangleShape( const pbrt::Vector3f& translation )
{
    const P3 minPt{ 0.0f, 0.0f, 0.0f };
    const P3 maxPt{ 1.0f, 1.0f, 1.0f };
    const B3 bounds{ minPt, maxPt };

    std::vector<P3> vertices{ P3{ 0.0f, 0.0f, 0.0f }, P3{ 1.0f, 0.0f, 0.0f }, P3{ 1.0f, 1.0f, 1.0f } };

    return { SHAPE_TYPE_TRIANGLE_MESH,
             Translate( translation ),
             expectedMaterial(),
             bounds,
             {},
             TriangleMeshData{ { 0, 1, 2 }, std::move( vertices ) } };
}

static ShapeDefinition singleTriangleShape()
{
    return translatedTriangleShape( pbrt::Vector3f{ 1.0f, 2.0f, 3.0f } );
}

static SceneDescriptionPtr singleTriangleScene()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeDefinition     mesh{ singleTriangleShape() };
    scene->bounds = transformBounds( mesh );
    scene->freeShapes.push_back( mesh );
    return scene;
}

static ShapeDefinition singleSphereShape()
{
    const P3 minPt{ 0.0f, 0.0f, 0.0f };
    const P3 maxPt{ 1.0f, 1.0f, 1.0f };
    const B3 bounds{ minPt, maxPt };

    PlasticMaterial material{};
    material.Ka = P3{ 0.1f, 0.2f, 0.3f };
    material.Kd = P3{ 0.4f, 0.5f, 0.6f };
    material.Ks = P3{ 0.7f, 0.8f, 0.9f };

    std::vector<P3> vertices{ P3{ 0.0f, 0.0f, 0.0f }, P3{ 1.0f, 0.0f, 0.0f }, P3{ 1.0f, 1.0f, 1.0f } };

    SphereData sphere;
    sphere.radius = 1.25f;
    sphere.zMin   = -sphere.radius;
    sphere.zMax   = sphere.radius;
    sphere.phiMax = 360.0f;

    pbrt::Vector3f translation{ 1.0f, 2.0f, 3.0f };
    return { SHAPE_TYPE_SPHERE, Translate( translation ), material, bounds, {}, {}, sphere };
}

static SceneDescriptionPtr singleSphereScene()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };

    ShapeDefinition shape{ singleSphereShape() };
    scene->freeShapes.push_back( shape );
    scene->bounds = transformBounds( shape );

    return scene;
}

static ShapeDefinition plyMeshShape( MockMeshLoaderPtr meshLoader )
{
    const P3 minPt{ 0.0f, 0.0f, 0.0f };
    const P3 maxPt{ 1.0f, 1.0f, 1.0f };
    const B3 bounds{ minPt, maxPt };

    PlasticMaterial material{};
    material.Ka = P3{ 0.1f, 0.2f, 0.3f };
    material.Kd = P3{ 0.4f, 0.5f, 0.6f };
    material.Ks = P3{ 0.7f, 0.8f, 0.9f };

    pbrt::Vector3f translation{ 1.0f, 2.0f, 3.0f };
    return { SHAPE_TYPE_PLY_MESH,
             Translate( translation ),
             material,
             bounds,
             PlyMeshData{ "cube-mesh.ply", meshLoader },
             {} };
}

static SceneDescriptionPtr singleTrianglePlyScene( MockMeshLoaderPtr meshLoader )
{
    ShapeDefinition     mesh{ plyMeshShape( meshLoader ) };
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    scene->bounds = transformBounds( mesh );
    scene->freeShapes.push_back( mesh );
    return scene;
}

static SceneDescriptionPtr singleTriangleWithNormalsScene()
{
    SceneDescriptionPtr scene{ singleTriangleScene() };
    std::vector<P3>     normals{ P3{ 0.1f, 0.2f, 0.3f }, P3{ 0.4f, 0.5f, 0.6f }, P3{ 0.7f, 0.8f, 0.9f } };
    scene->freeShapes[0].triangleMesh.normals = std::move( normals );
    return scene;
}

static SceneDescriptionPtr singleTriangleWithUVsScene()
{
    SceneDescriptionPtr scene{ singleTriangleScene() };
    std::vector<P2>     uvs{ P2{ 0.0f, 0.0f }, P2{ 1.0f, 0.0f }, P2{ 1.0f, 1.0f } };
    scene->freeShapes[0].triangleMesh.uvs = std::move( uvs );
    return scene;
}

static SceneDescriptionPtr singleTriangleWithAlphaMapScene()
{
    SceneDescriptionPtr scene{ singleTriangleWithUVsScene() };
    scene->freeShapes[0].material.alphaMapFileName = "alphaMap.png";
    return scene;
}

static SceneDescriptionPtr singleTriangleWithDiffuseMapScene()
{
    SceneDescriptionPtr scene{ singleTriangleWithUVsScene() };
    scene->freeShapes[0].material.diffuseMapFileName = "diffuse.png";
    return scene;
}

static SceneDescriptionPtr twoShapeScene()
{
    ShapeDefinition shape1{ translatedTriangleShape( pbrt::Vector3f{ 1.0f, 2.0f, 3.0f } ) };
    ShapeDefinition shape2{ translatedTriangleShape( pbrt::Vector3f{ -1.0f, -2.0f, -3.0f } ) };

    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    scene->bounds = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    scene->freeShapes.push_back( shape1 );
    scene->freeShapes.push_back( shape2 );
    return scene;
}

static SceneDescriptionPtr singleInstanceSingleShapeScene()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeDefinition     shape{ singleTriangleShape() };
    ObjectDefinition    object;
    object.bounds                     = transformBounds( shape );
    scene->objects["triangle"]        = object;
    scene->instanceCounts["triangle"] = 1;
    ObjectInstanceDefinition instance;
    instance.name   = "triangle";
    instance.bounds = object.bounds;
    scene->objectInstances.push_back( instance );
    ShapeList shapeList;
    shapeList.push_back( shape );
    scene->objectShapes["triangle"] = shapeList;
    scene->bounds                   = transformBounds( instance );
    return scene;
}

static SceneDescriptionPtr singleInstanceMultipleShapesScene()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeDefinition     shape1{ translatedTriangleShape( pbrt::Vector3f{ 1.0f, 2.0f, 3.0f } ) };
    ShapeDefinition     shape2{ translatedTriangleShape( pbrt::Vector3f{ -1.0f, -2.0f, -3.0f } ) };
    ObjectDefinition    object;
    std::string         name{ "object" };
    object.bounds               = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    scene->objects[name]        = object;
    scene->instanceCounts[name] = 1;
    ObjectInstanceDefinition instance;
    instance.name   = name;
    instance.bounds = object.bounds;
    scene->objectInstances.push_back( instance );
    ShapeList shapeList;
    shapeList.push_back( shape1 );
    shapeList.push_back( shape2 );
    scene->objectShapes[name] = shapeList;
    scene->bounds             = transformBounds( instance );
    return scene;
}

static SceneDescriptionPtr singleInstanceSingleShapeSingleFreeShapeScene()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeDefinition     shape1{ translatedTriangleShape( pbrt::Vector3f{ 1.0f, 2.0f, 3.0f } ) };
    ObjectDefinition    object;
    object.bounds = transformBounds( shape1 );
    std::string name{ "object" };
    scene->objects[name]        = object;
    scene->instanceCounts[name] = 1;
    ObjectInstanceDefinition instance;
    instance.name      = name;
    instance.transform = Translate( pbrt::Vector3f( -5.0f, -10.0f, -15.0f ) );
    instance.bounds    = object.bounds;
    scene->objectInstances.push_back( instance );
    ShapeList shapeList;
    shapeList.push_back( shape1 );
    scene->objectShapes[name] = shapeList;

    ShapeDefinition shape2{ translatedTriangleShape( pbrt::Vector3f{ -1.0f, -2.0f, -3.0f } ) };
    scene->freeShapes.push_back( shape2 );
    scene->bounds = Union( transformBounds( instance ), transformBounds( shape2 ) );
    return scene;
}

static SceneDescriptionPtr multipleInstancesSingleShape()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeDefinition     shape{ translatedTriangleShape( pbrt::Vector3f{ 1.0f, 2.0f, 3.0f } ) };
    ObjectDefinition    object;
    object.bounds = transformBounds( shape );
    std::string name{ "object" };
    scene->objects[name] = object;
    ShapeList shapeList;
    shapeList.push_back( shape );
    scene->objectShapes[name] = shapeList;
    const auto createInstance = [&]( const pbrt::Vector3f& translation ) {
        ObjectInstanceDefinition instance;
        instance.name      = name;
        instance.transform = Translate( translation );
        instance.bounds    = object.bounds;
        scene->objectInstances.push_back( instance );
        scene->instanceCounts[name]++;
    };
    createInstance( pbrt::Vector3f( -5.0f, -10.0f, -15.0f ) );
    createInstance( pbrt::Vector3f( 10.0f, 10.0f, 10.0f ) );

    const ObjectInstanceDefinition& ins1{ scene->objectInstances[0] };
    const ObjectInstanceDefinition& ins2{ scene->objectInstances[1] };
    scene->bounds = Union( transformBounds( ins1 ), transformBounds( ins2 ) );
    return scene;
}

static SceneDescriptionPtr singleInstanceTwoTriangleShapeScene()
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeList           shapeList;
    shapeList.push_back( singleTriangleShape() );
    shapeList.push_back( singleTriangleShape() );
    ShapeDefinition& shape1{ shapeList[0] };
    ShapeDefinition& shape2{ shapeList[1] };
    shape2.transform = Translate( ::pbrt::Vector3f( 1.0f, 1.0f, 1.0f ) );
    shape2.material  = expectedSecondMaterial();
    ObjectDefinition object;
    object.bounds = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    std::string objectName{ "triangle" };
    scene->objects[objectName]        = object;
    scene->instanceCounts[objectName] = 1;
    ObjectInstanceDefinition instance;
    instance.name   = objectName;
    instance.bounds = object.bounds;
    scene->objectInstances.push_back( instance );
    scene->objectShapes[objectName] = shapeList;
    scene->bounds                   = transformBounds( instance );
    return scene;
}

static SceneDescriptionPtr singleInstanceTriangleMesShapePlyMeshShapeScene( MockMeshLoaderPtr meshLoader )
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeList           shapeList;
    shapeList.push_back( singleTriangleShape() );
    shapeList.push_back( plyMeshShape( meshLoader ) );
    ShapeDefinition& shape1{ shapeList[0] };
    ShapeDefinition& shape2{ shapeList[1] };
    shape2.transform = Translate( ::pbrt::Vector3f( 1.0f, 1.0f, 1.0f ) );
    ObjectDefinition object;
    object.bounds = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    std::string name{ "triangle" };
    scene->objects[name]        = object;
    scene->instanceCounts[name] = 1;
    ObjectInstanceDefinition instance;
    instance.name   = name;
    instance.bounds = object.bounds;
    scene->objectInstances.push_back( instance );
    scene->objectShapes[name] = shapeList;
    scene->bounds             = transformBounds( instance );
    return scene;
}

static SceneDescriptionPtr singleInstanceTwoTriangleMixedMaterialTypesShapeScene( const std::string& name )
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeList           shapeList;
    shapeList.push_back( singleTriangleShape() );
    shapeList.push_back( singleTriangleShape() );
    ShapeDefinition& shape1{ shapeList[0] };
    ShapeDefinition& shape2{ shapeList[1] };
    shape2.transform = Translate( ::pbrt::Vector3f( 1.0f, 1.0f, 1.0f ) );
    const std::array<P2, 3> uvs{ P2( 0.0f, 0.0f ), P2( 0.0f, 1.0f ), P2( 0.0f, 0.0f ) };
    // different SBT indices are needed for textured and non-textured shapes
    std::copy( uvs.begin(), uvs.end(), std::back_inserter( shape1.triangleMesh.uvs ) );
    shape1.material.alphaMapFileName   = ALPHA_MAP_FILENAME;
    shape1.material.diffuseMapFileName = DIFFUSE_MAP_FILENAME;
    ObjectDefinition object;
    object.bounds               = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    scene->objects[name]        = object;
    scene->instanceCounts[name] = 1;
    ObjectInstanceDefinition instance;
    instance.name   = name;
    instance.bounds = object.bounds;
    scene->objectInstances.push_back( instance );
    scene->objectShapes[name] = shapeList;
    scene->bounds             = transformBounds( instance );
    return scene;
}

// different SBT indices are needed for alpha textured and alpha+diffuse textured shapes
static SceneDescriptionPtr singleInstanceThreeTriangleMixedTextureTypesShapeScene( const std::string& name )
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeList           shapeList;
    shapeList.push_back( singleTriangleShape() );
    shapeList.push_back( singleTriangleShape() );
    shapeList.push_back( singleTriangleShape() );
    ShapeDefinition&        shape1{ shapeList[0] };
    ShapeDefinition&        shape2{ shapeList[1] };
    ShapeDefinition&        shape3{ shapeList[2] };
    const std::array<P2, 3> uvs{ P2( 0.0f, 0.0f ), P2( 0.0f, 1.0f ), P2( 0.0f, 0.0f ) };

    std::copy( uvs.begin(), uvs.end(), std::back_inserter( shape1.triangleMesh.uvs ) );
    shape1.material.alphaMapFileName   = ALPHA_MAP_FILENAME;
    shape1.material.diffuseMapFileName = DIFFUSE_MAP_FILENAME;

    shape2.transform = Translate( ::pbrt::Vector3f( 1.0f, 1.0f, 1.0f ) );
    std::copy( uvs.begin(), uvs.end(), std::back_inserter( shape2.triangleMesh.uvs ) );
    shape2.material.alphaMapFileName = ALPHA_MAP_FILENAME;

    shape3.transform = Translate( ::pbrt::Vector3f( 2.0f, 2.0f, 2.0f ) );
    std::copy( uvs.begin(), uvs.end(), std::back_inserter( shape3.triangleMesh.uvs ) );
    shape3.material.diffuseMapFileName = DIFFUSE_MAP_FILENAME;

    ObjectDefinition object;
    object.bounds               = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    scene->objects[name]        = object;
    scene->instanceCounts[name] = 1;
    ObjectInstanceDefinition instance;
    instance.name   = name;
    instance.bounds = object.bounds;
    scene->objectInstances.push_back( instance );
    scene->objectShapes[name] = shapeList;
    scene->bounds             = transformBounds( instance );
    return scene;
}

static SceneDescriptionPtr singleInstanceOneTriangleOneSphereShapeScene( const std::string& objectName )
{
    SceneDescriptionPtr scene{ std::make_shared<SceneDescription>() };
    ShapeList           shapeList;
    shapeList.push_back( singleTriangleShape() );
    shapeList.push_back( singleSphereShape() );
    ShapeDefinition& shape1{ shapeList[0] };
    ShapeDefinition& shape2{ shapeList[1] };
    shape2.transform = Translate( ::pbrt::Vector3f( 1.0f, 1.0f, 1.0f ) );
    ObjectDefinition object;
    object.bounds                     = Union( transformBounds( shape1 ), transformBounds( shape2 ) );
    scene->objects[objectName]        = object;
    scene->instanceCounts[objectName] = 1;
    ObjectInstanceDefinition instance;
    instance.name      = objectName;
    instance.transform = Translate( ::pbrt::Vector3f( 10.0f, 10.0f, 10.0f ) );
    instance.bounds    = instance.transform( object.bounds );
    scene->objectInstances.push_back( instance );
    scene->objectShapes[objectName] = shapeList;
    scene->bounds                   = transformBounds( instance );
    return scene;
}

static void identity( float ( &result )[12] )
{
    static const float matrix[12]{
        1.0f, 0.0f, 0.0f, 0.0f,  //
        0.0f, 1.0f, 0.0f, 0.0f,  //
        0.0f, 0.0f, 1.0f, 0.0f   //
    };
    std::copy( std::begin( matrix ), std::end( matrix ), std::begin( result ) );
}

namespace otk {
namespace pbrt {

inline bool operator==( const ObjectDefinition& lhs, const ObjectDefinition& rhs )
{
    return lhs.name == rhs.name          //
           && lhs.bounds == rhs.bounds;  //
}

inline std::ostream& operator<<( std::ostream& str, const ObjectDefinition& value )
{
    return str << "ObjectDefinition{ '" << value.name << "', " << value.bounds << " }";
}

inline bool operator==( const PlyMeshData& lhs, const PlyMeshData& rhs )
{
    return lhs.fileName == rhs.fileName && lhs.loader == rhs.loader;
}

inline bool operator==( const TriangleMeshData& lhs, const TriangleMeshData& rhs )
{
    return lhs.indices == rhs.indices && lhs.points == rhs.points && lhs.normals == rhs.normals && lhs.uvs == rhs.uvs;
}

inline bool operator==( const SphereData& lhs, const SphereData& rhs )
{
    return lhs.radius == rhs.radius && lhs.zMin == rhs.zMin && lhs.zMax == rhs.zMax && lhs.phiMax == rhs.phiMax;
}

inline bool operator==( const ShapeDefinition& lhs, const ShapeDefinition& rhs )
{
    if( lhs.type != rhs.type )
        return false;

    if( lhs.type == SHAPE_TYPE_PLY_MESH )
        return lhs.plyMesh == rhs.plyMesh;

    if( lhs.type == SHAPE_TYPE_TRIANGLE_MESH )
        return lhs.triangleMesh == rhs.triangleMesh;

    if( lhs.type == SHAPE_TYPE_SPHERE )
        return lhs.sphere == rhs.sphere;

    return false;
}

}  // namespace pbrt
}  // namespace otk

TEST( TestSceneConstruction, sceneBoundsSingleTriangleScene )
{
    SceneDescriptionPtr scene{ singleTriangleScene() };

    const ShapeList& shapes{ scene->freeShapes };
    EXPECT_EQ( 1U, shapes.size() );
    const ShapeDefinition& shape{ shapes[0] };
    EXPECT_EQ( scene->bounds, transformBounds( shape ) );
}

TEST( TestSceneConstruction, sceneBoundsSingleSphereScene )
{
    SceneDescriptionPtr scene{ singleSphereScene() };

    const ShapeList& shapes{ scene->freeShapes };
    EXPECT_EQ( 1U, shapes.size() );
    const ShapeDefinition& shape{ shapes[0] };
    EXPECT_EQ( scene->bounds, transformBounds( shape ) );
}

namespace {

class MockGeometryCache : public StrictMock<GeometryCache>
{
  public:
    ~MockGeometryCache() override = default;

    MOCK_METHOD( GeometryCacheEntry, getShape, (OptixDeviceContext, CUstream, const ShapeDefinition&), ( override ) );
    MOCK_METHOD( GeometryCacheEntry,
                 getObject,
                 ( OptixDeviceContext      context,
                   CUstream                stream,
                   const ObjectDefinition& object,
                   const ShapeList&        shapes,
                   GeometryPrimitive       primitive,
                   MaterialFlags           flags ) );
    MOCK_METHOD( GeometryCacheStatistics, getStatistics, (), ( const override ) );
};

using MockGeometryCachePtr = std::shared_ptr<MockGeometryCache>;

}  // namespace

TEST( TestSceneConstruction, sceneBoundsSingleTrianglePlyScene )
{
    MockMeshLoaderPtr   meshLoader{ createMockMeshLoader() };
    SceneDescriptionPtr scene{ singleTrianglePlyScene( meshLoader ) };

    const ShapeList& shapes{ scene->freeShapes };
    EXPECT_EQ( 1U, shapes.size() );
    const ShapeDefinition& shape{ shapes[0] };
    EXPECT_EQ( scene->bounds, transformBounds( shape ) );
}

TEST( TestSceneConstruction, meshDataSingleTrianglePlyScene )
{
    MockMeshLoaderPtr   meshLoader{ createMockMeshLoader() };
    SceneDescriptionPtr scene{ singleTrianglePlyScene( meshLoader ) };

    const ShapeDefinition& shape{ scene->freeShapes[0] };
    EXPECT_EQ( std::string{ SHAPE_TYPE_PLY_MESH }, shape.type );
    EXPECT_EQ( "cube-mesh.ply", shape.plyMesh.fileName );
    EXPECT_EQ( meshLoader, shape.plyMesh.loader );
}

TEST( TestSceneConstruction, constructSingleTriangleWithNormalsScene )
{
    SceneDescriptionPtr scene{ singleTriangleWithNormalsScene() };

    ASSERT_FALSE( scene->freeShapes.empty() );
    const ShapeDefinition& shape{ scene->freeShapes[0] };
    EXPECT_EQ( SHAPE_TYPE_TRIANGLE_MESH, shape.type );
    const TriangleMeshData& mesh{ shape.triangleMesh };
    EXPECT_FALSE( mesh.normals.empty() );
}

TEST( TestSceneConstruction, constructSingleTriangleWithUVsScene )
{
    SceneDescriptionPtr scene{ singleTriangleWithUVsScene() };

    ASSERT_FALSE( scene->freeShapes.empty() );
    const ShapeDefinition& shape{ scene->freeShapes[0] };
    EXPECT_EQ( SHAPE_TYPE_TRIANGLE_MESH, shape.type );
    const TriangleMeshData& mesh{ shape.triangleMesh };
    EXPECT_FALSE( mesh.uvs.empty() );
}

TEST( TestSceneConstruction, constructSingleTriangleWithAlphaMapScene )
{
    SceneDescriptionPtr scene{ singleTriangleWithAlphaMapScene() };

    ASSERT_FALSE( scene->freeShapes.empty() );
    const ShapeDefinition& shape{ scene->freeShapes[0] };
    EXPECT_FALSE( shape.material.alphaMapFileName.empty() );
}

TEST( TestSceneConstruction, constructSingleDiffuseMapTriangleScene )
{
    SceneDescriptionPtr scene{ singleTriangleWithDiffuseMapScene() };

    ASSERT_FALSE( scene->freeShapes.empty() );
    const ShapeDefinition& shape{ scene->freeShapes[0] };
    EXPECT_FALSE( shape.material.diffuseMapFileName.empty() );
}

TEST( TestSceneConstruction, sceneBoundsTwoShapeScene )
{
    SceneDescriptionPtr scene{ twoShapeScene() };

    const ShapeList& shapes{ scene->freeShapes };
    EXPECT_EQ( 2U, shapes.size() );
    const ShapeDefinition& shape1{ shapes[0] };
    const B3               shape1WorldBounds{ transformBounds( shape1 ) };
    EXPECT_TRUE( Overlaps( shape1WorldBounds, scene->bounds ) );
    const ShapeDefinition& shape2{ shapes[1] };
    const B3               shape2WorldBounds{ transformBounds( shape2 ) };
    EXPECT_TRUE( Overlaps( shape2WorldBounds, scene->bounds ) );
    EXPECT_EQ( scene->bounds, Union( shape1WorldBounds, shape2WorldBounds ) );
}

TEST( TestSceneConstruction, sceneBoundsSingleInstanceSingleShapeScene )
{
    SceneDescriptionPtr scene{ singleInstanceSingleShapeScene() };

    const ShapeList& shapes{ scene->objectShapes["triangle"] };
    B3               expectedInstanceBounds{ transformBounds( shapes[0] ) };
    EXPECT_EQ( expectedInstanceBounds, scene->objectInstances[0].bounds );
    EXPECT_EQ( scene->objectInstances[0].transform( expectedInstanceBounds ), scene->bounds );
}

TEST( TestSceneConstruction, sceneBoundsSingleInstanceMultipleShapesScene )
{
    SceneDescriptionPtr scene{ singleInstanceMultipleShapesScene() };

    const ShapeList& shapes{ scene->objectShapes["object"] };
    B3               expectedInstanceBounds{ Union( transformBounds( shapes[0] ), transformBounds( shapes[1] ) ) };
    EXPECT_EQ( expectedInstanceBounds, scene->objectInstances[0].bounds );
    EXPECT_EQ( scene->objectInstances[0].transform( expectedInstanceBounds ), scene->bounds );
}

TEST( TestSceneConstruction, sceneBoundsSingleInstanceSingleShapeSingleFreeShapeScene )
{
    SceneDescriptionPtr scene{ singleInstanceSingleShapeSingleFreeShapeScene() };

    const ShapeList freeShapes{ scene->freeShapes };
    ASSERT_FALSE( freeShapes.empty() );
    const ShapeList& instanceShapes{ scene->objectShapes["object"] };
    ASSERT_FALSE( instanceShapes.empty() );
    B3 expectedInstanceBounds{ transformBounds( instanceShapes[0] ) };
    EXPECT_EQ( expectedInstanceBounds, scene->objectInstances[0].bounds );
    B3 expectedFreeShapeBounds{ transformBounds( freeShapes[0] ) };
    EXPECT_TRUE( Overlaps( expectedFreeShapeBounds, scene->bounds ) );
    B3 expectedObjectInstanceBounds{ scene->objectInstances[0].transform( expectedInstanceBounds ) };
    EXPECT_TRUE( Overlaps( expectedObjectInstanceBounds, scene->bounds ) )
        << expectedObjectInstanceBounds << " not in " << scene->bounds;
}

TEST( TestSceneConstruction, sceneBoundsMultipleInstancesSingleShape )
{
    SceneDescriptionPtr scene{ multipleInstancesSingleShape() };

    ASSERT_TRUE( scene->freeShapes.empty() );
    const ShapeList& instanceShapes{ scene->objectShapes["object"] };
    ASSERT_FALSE( instanceShapes.empty() );
    B3 expectedShapeBounds{ transformBounds( instanceShapes[0] ) };
    EXPECT_EQ( expectedShapeBounds, scene->objectInstances[0].bounds );
    EXPECT_EQ( expectedShapeBounds, scene->objectInstances[1].bounds );
    B3 ins1Bounds{ transformBounds( scene->objectInstances[0] ) };
    B3 ins2Bounds{ transformBounds( scene->objectInstances[1] ) };
    EXPECT_NE( ins1Bounds, ins2Bounds );
    EXPECT_TRUE( Overlaps( ins1Bounds, scene->bounds ) ) << ins1Bounds << " not in " << scene->bounds;
    EXPECT_TRUE( Overlaps( ins2Bounds, scene->bounds ) ) << ins2Bounds << " not in " << scene->bounds;
}

namespace {

class TestSceneProxy : public Test
{
  protected:
    void SetUp() override
    {
        m_options.proxyGranularity     = ProxyGranularity::FINE;
        m_accelSizes.tempSizeInBytes   = 1234U;
        m_accelSizes.outputSizeInBytes = 5678U;
    }

    Expectation expectProxyBoundsAdded( const ::B3& bounds, uint_t pageId ) const
    {
        return EXPECT_CALL( *m_geometryLoader, add( toOptixAabb( bounds ) ) ).WillOnce( Return( pageId ) );
    }
    template <typename Thing>
    Expectation expectProxyAdded( const Thing& thing, uint_t pageId )
    {
        return expectProxyBoundsAdded( transformBounds( thing ), pageId );
    }
    Expectation expectProxyBoundsAddedAfter( const ::B3& bounds, uint_t pageId, ExpectationSet before ) const
    {
        return EXPECT_CALL( *m_geometryLoader, add( toOptixAabb( bounds ) ) ).After( before ).WillOnce( Return( pageId ) );
    }
    template <typename Thing>
    Expectation expectProxyAddedAfter( const Thing& thing, uint_t pageId, ExpectationSet before )
    {
        return expectProxyBoundsAddedAfter( transformBounds( thing ), pageId, before );
    }

    GeometryCacheEntry expectShapeFromCache( const ShapeDefinition& shape )
    {
        GeometryCacheEntry entry{};
        entry.accelBuffer = CUdeviceptr{ 0xf00dbaadf00dbaadULL };
        entry.traversable = m_fakeGeometryAS;
        if( shape.type == SHAPE_TYPE_TRIANGLE_MESH )
        {
            if( !shape.triangleMesh.normals.empty() )
            {
                entry.devNormals = otk::bit_cast<TriangleNormals*>( 0xbaadf00dbaaabaaaULL );
            }
            if( !shape.triangleMesh.uvs.empty() )
            {
                entry.devUVs = otk::bit_cast<TriangleUVs*>( 0xbaaabaaaf00dbaadULL );
            }
        }
        entry.primitiveGroupEndIndices.push_back( ARBITRARY_PRIMITIVE_GROUP_END );
        EXPECT_CALL( *m_geometryCache, getShape( m_fakeContext, m_stream, shape ) ).WillOnce( Return( entry ) );
        return entry;
    }

    CUstream               m_stream{ otk::bit_cast<CUstream>( 0xbaadfeedfeedfeedULL ) };
    uint_t                 m_pageId{ 10 };
    MockGeometryLoaderPtr  m_geometryLoader{ createMockGeometryLoader() };
    MockGeometryCachePtr   m_geometryCache{ std::make_shared<MockGeometryCache>() };
    Options                m_options{};
    ProxyFactoryPtr        m_factory{ createProxyFactory( m_options, m_geometryLoader, m_geometryCache ) };
    SceneProxyPtr          m_proxy;
    SceneDescriptionPtr    m_scene;
    OptixDeviceContext     m_fakeContext{ otk::bit_cast<OptixDeviceContext>( 0xf00df00dULL ) };
    OptixAccelBufferSizes  m_accelSizes{};
    OptixTraversableHandle m_fakeGeometryAS{ 0xfeedf00dU };
};

}  // namespace

TEST_F( TestSceneProxy, constructWholeSceneProxyForSingleTriangleMesh )
{
    m_scene = singleTriangleScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );

    m_proxy = m_factory->scene( m_scene );

    ASSERT_TRUE( m_proxy );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    const OptixAabb expectedBounds{ toOptixAabb( m_scene->bounds ) };
    EXPECT_EQ( expectedBounds, m_proxy->getBounds() ) << expectedBounds << " ! " << m_proxy->getBounds();
    EXPECT_FALSE( m_proxy->isDecomposable() );
    const ProxyFactoryStatistics stats{ m_factory->getStatistics() };
    EXPECT_EQ( 1, stats.numGeometryProxiesCreated );
}

TEST_F( TestSceneProxy, constructTriangleASForSingleTriangleMesh )
{
    m_scene = singleTriangleScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, constructTriangleASForSingleTriangleMeshWithNormals )
{
    m_scene = singleTriangleWithNormalsScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( entry.devNormals, geom.devNormals );
    EXPECT_NE( nullptr, geom.devNormals );
    EXPECT_EQ( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, constructTriangleASForSingleTriangleMeshWithUVs )
{
    m_scene = singleTriangleWithUVsScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( entry.devUVs, geom.devUVs );
    EXPECT_NE( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, constructTriangleASForSingleTriangleMeshWithAlphaMap )
{
    m_scene = singleTriangleWithAlphaMapScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( entry.devUVs, geom.devUVs );
    EXPECT_NE( nullptr, geom.devUVs );
    EXPECT_FALSE( geom.groups[0].alphaMapFileName.empty() );
    EXPECT_TRUE( geom.groups[0].diffuseMapFileName.empty() );
    EXPECT_EQ( geom.groups[0].material.flags, MaterialFlags::ALPHA_MAP );
}

TEST_F( TestSceneProxy, constructTriangleASForSingleTriangleMeshWithDiffuseMap )
{
    m_scene = singleTriangleWithDiffuseMapScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( entry.devUVs, geom.devUVs );
    EXPECT_NE( nullptr, geom.devUVs );
    EXPECT_TRUE( geom.groups[0].alphaMapFileName.empty() );
    EXPECT_FALSE( geom.groups[0].diffuseMapFileName.empty() );
    EXPECT_EQ( geom.groups[0].material.flags, MaterialFlags::DIFFUSE_MAP );
}

TEST_F( TestSceneProxy, constructWholeSceneProxyForMultipleShapes )
{
    m_scene = twoShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );

    m_proxy = m_factory->scene( m_scene );

    ASSERT_TRUE( m_proxy );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    EXPECT_EQ( toOptixAabb( m_scene->bounds ), m_proxy->getBounds() );
    EXPECT_TRUE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, decomposeProxyForMultipleShapes )
{
    m_scene = twoShapeScene();
    EXPECT_EQ( m_scene->freeShapes[0].bounds, m_scene->freeShapes[1].bounds );
    EXPECT_NE( m_scene->freeShapes[0].transform, m_scene->freeShapes[1].transform );
    ExpectationSet first{ expectProxyBoundsAdded( m_scene->bounds, m_pageId ) };
    m_proxy = m_factory->scene( m_scene );
    const uint_t shape1PageId{ 1111 };
    const uint_t shape2PageId{ 2222 };
    expectProxyAdded( m_scene->freeShapes[0], shape1PageId );
    expectProxyAdded( m_scene->freeShapes[1], shape2PageId );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    ASSERT_FALSE( parts.empty() );
    EXPECT_TRUE( std::none_of( parts.begin(), parts.end(), []( SceneProxyPtr proxy ) { return proxy->isDecomposable(); } ) );
    EXPECT_EQ( shape1PageId, parts[0]->getPageId() );
    EXPECT_EQ( shape2PageId, parts[1]->getPageId() );
    const auto transformedBounds = [&]( int index ) {
        return toOptixAabb( transformBounds( m_scene->freeShapes[index] ) );
    };
    const OptixAabb expectedBounds1{ transformedBounds( 0 ) };
    EXPECT_EQ( expectedBounds1, parts[0]->getBounds() ) << expectedBounds1 << " != " << parts[0]->getBounds();
    const OptixAabb expectedBounds2{ transformedBounds( 1 ) };
    EXPECT_EQ( expectedBounds2, parts[1]->getBounds() ) << expectedBounds2 << " != " << parts[1]->getBounds();
}

TEST_F( TestSceneProxy, constructTriangleASForSecondMesh )
{
    m_scene = twoShapeScene();
    ExpectationSet first{ expectProxyBoundsAdded( m_scene->bounds, m_pageId ) };
    m_proxy = m_factory->scene( m_scene );
    const uint_t   shape1PageId{ 1111 };
    const uint_t   shape2PageId{ 2222 };
    ExpectationSet second;
    second += expectProxyAddedAfter( m_scene->freeShapes[0], shape1PageId, first );
    second += expectProxyAddedAfter( m_scene->freeShapes[1], shape2PageId, first );
    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };
    GeometryCacheEntry         entry{ expectShapeFromCache( m_scene->freeShapes[1] ) };
    float                      expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = -1.0f;
    expectedTransform[7]  = -2.0f;
    expectedTransform[11] = -3.0f;

    const GeometryInstance geom{ parts[1]->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, constructWholeSceneProxyForSingleInstanceWithSingleShape )
{
    m_scene = singleInstanceSingleShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );

    m_proxy = m_factory->scene( m_scene );

    ASSERT_TRUE( m_proxy );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    EXPECT_EQ( toOptixAabb( m_scene->bounds ), m_proxy->getBounds() );
    EXPECT_FALSE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, geometryForSingleInstanceWithSingleShape )
{
    m_scene = singleInstanceSingleShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->objectShapes["triangle"][0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( nullptr, entry.devNormals );
    EXPECT_EQ( nullptr, entry.devUVs );
    EXPECT_NE( CUdeviceptr{}, entry.accelBuffer );
    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, constructWholeSceneProxyForSingleInstanceWithMultipleShape )
{
    m_scene = singleInstanceMultipleShapesScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );

    m_proxy = m_factory->scene( m_scene );

    ASSERT_TRUE( m_proxy );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    const OptixAabb expectedBounds{ toOptixAabb( m_scene->bounds ) };
    EXPECT_EQ( expectedBounds, m_proxy->getBounds() ) << expectedBounds << " != " << m_proxy->getBounds();
    EXPECT_TRUE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, decomposeWholeSceneProxyForSingleInstanceWithMultipleShape )
{
    m_scene = singleInstanceMultipleShapesScene();
    ExpectationSet first{ expectProxyBoundsAdded( m_scene->bounds, m_pageId ) };
    m_proxy = m_factory->scene( m_scene );
    const uint_t     shape1PageId{ 1111 };
    const uint_t     shape2PageId{ 2222 };
    const ShapeList& objectShapes{ m_scene->objectShapes["object"] };
    expectProxyAddedAfter( objectShapes[0], shape1PageId, first );
    expectProxyAddedAfter( objectShapes[1], shape2PageId, first );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    ASSERT_FALSE( parts.empty() );
    EXPECT_TRUE( std::none_of( parts.begin(), parts.end(), []( SceneProxyPtr proxy ) { return proxy->isDecomposable(); } ) );
    EXPECT_EQ( shape1PageId, parts[0]->getPageId() );
    EXPECT_EQ( shape2PageId, parts[1]->getPageId() );
    const auto transformedBounds{ [&]( int index ) { return toOptixAabb( transformBounds( objectShapes[index] ) ); } };
    EXPECT_EQ( transformedBounds( 0 ), parts[0]->getBounds() ) << transformedBounds( 0 ) << " != " << parts[0]->getBounds();
    EXPECT_EQ( transformedBounds( 1 ), parts[1]->getBounds() ) << transformedBounds( 1 ) << " != " << parts[1]->getBounds();
}

TEST_F( TestSceneProxy, constructWholeSceneProxyForSingleInstanceAndSingleFreeShape )
{
    m_scene = singleInstanceSingleShapeSingleFreeShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );

    m_proxy = m_factory->scene( m_scene );

    ASSERT_TRUE( m_proxy );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    const OptixAabb expectedBounds{ toOptixAabb( m_scene->bounds ) };
    EXPECT_EQ( expectedBounds, m_proxy->getBounds() ) << expectedBounds << " != " << m_proxy->getBounds();
    EXPECT_TRUE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, decomposeWholeSceneProxyForSingleInstanceSingleShapeSingleFreeShapeScene )
{
    m_scene = singleInstanceSingleShapeSingleFreeShapeScene();
    ExpectationSet first{ expectProxyBoundsAdded( m_scene->bounds, m_pageId ) };
    m_proxy = m_factory->scene( m_scene );
    const uint_t shape1PageId{ 1111 };
    const uint_t shape2PageId{ 2222 };
    expectProxyAddedAfter( m_scene->objectInstances[0], shape1PageId, first );
    expectProxyAddedAfter( m_scene->freeShapes[0], shape2PageId, first );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    ASSERT_EQ( 2U, parts.size() );
    EXPECT_TRUE( std::none_of( parts.begin(), parts.end(), []( SceneProxyPtr proxy ) { return proxy->isDecomposable(); } ) );
    EXPECT_EQ( shape1PageId, parts[0]->getPageId() );
    EXPECT_EQ( shape2PageId, parts[1]->getPageId() );
    EXPECT_FALSE( parts[0]->isDecomposable() );
    EXPECT_FALSE( parts[1]->isDecomposable() );
    const OptixAabb instanceBounds{ toOptixAabb( transformBounds( m_scene->objectInstances[0] ) ) };
    EXPECT_EQ( instanceBounds, parts[0]->getBounds() ) << instanceBounds << " != " << parts[0]->getBounds();
    const OptixAabb freeShapeBounds{ toOptixAabb( transformBounds( m_scene->freeShapes[0] ) ) };
    EXPECT_EQ( freeShapeBounds, parts[1]->getBounds() ) << freeShapeBounds << " != " << parts[1]->getBounds();
}

TEST_F( TestSceneProxy, constructTriangleASForSinglePlyMesh )
{
    m_scene = singleTrianglePlyScene( createMockMeshLoader() );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_TRIANGLE, geom.instance.sbtOffset );
    EXPECT_EQ( m_fakeGeometryAS, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, multipleInstancesSingleShapeGeometry )
{
    m_scene = multipleInstancesSingleShape();
    ExpectationSet first{ expectProxyBoundsAdded( m_scene->bounds, 1111 ) };
    m_proxy = m_factory->scene( m_scene );
    expectProxyAddedAfter( m_scene->objectInstances[0], 2222, first );
    expectProxyAddedAfter( m_scene->objectInstances[1], 3333, first );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    const OptixAabb shape1Bounds{ toOptixAabb( transformBounds( m_scene->objectInstances[0] ) ) };
    const OptixAabb shape2Bounds{ toOptixAabb( transformBounds( m_scene->objectInstances[1] ) ) };
    EXPECT_EQ( shape1Bounds, parts[0]->getBounds() ) << shape1Bounds << " != " << parts[0]->getBounds();
    EXPECT_EQ( shape2Bounds, parts[1]->getBounds() ) << shape2Bounds << " != " << parts[1]->getBounds();
}

TEST_F( TestSceneProxy, constructProxyForSingleSphere )
{
    m_scene = singleSphereScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );

    m_proxy = m_factory->scene( m_scene );

    ASSERT_TRUE( m_proxy );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    const OptixAabb expectedBounds{ toOptixAabb( m_scene->bounds ) };
    EXPECT_EQ( expectedBounds, m_proxy->getBounds() ) << expectedBounds << " != " << m_proxy->getBounds();
    EXPECT_FALSE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, constructSphereASForSingleSphere )
{
    m_scene = singleSphereScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->scene( m_scene );
    const GeometryCacheEntry entry{ expectShapeFromCache( m_scene->freeShapes[0] ) };
    float                    expectedTransform[12];
    identity( expectedTransform );
    expectedTransform[3]  = 1.0f;
    expectedTransform[7]  = 2.0f;
    expectedTransform[11] = 3.0f;

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    EXPECT_EQ( entry.accelBuffer, geom.accelBuffer );
    EXPECT_TRUE( isSameTransform( expectedTransform, geom.instance.transform ) );
    EXPECT_EQ( +HitGroupIndex::PROXY_MATERIAL_SPHERE, geom.instance.sbtOffset );
    EXPECT_EQ( entry.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( 255U, geom.instance.visibilityMask );
    ASSERT_EQ( 1U, geom.groups.size() );
    EXPECT_EQ( make_float3( 0.1f, 0.2f, 0.3f ), geom.groups[0].material.Ka );
    EXPECT_EQ( make_float3( 0.4f, 0.5f, 0.6f ), geom.groups[0].material.Kd );
    EXPECT_EQ( make_float3( 0.7f, 0.8f, 0.9f ), geom.groups[0].material.Ks );
    EXPECT_EQ( ARBITRARY_PRIMITIVE_GROUP_END, geom.groups[0].primitiveIndexEnd );
    EXPECT_EQ( nullptr, geom.devNormals );
    EXPECT_EQ( nullptr, geom.devUVs );
}

TEST_F( TestSceneProxy, fineObjectInstanceDecomposable )
{
    m_options.proxyGranularity = ProxyGranularity::FINE;
    m_scene                    = singleInstanceTwoTriangleShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    EXPECT_TRUE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, fineObjectInstanceCreateGeometryIsError )
{
    m_options.proxyGranularity = ProxyGranularity::FINE;
    m_scene                    = singleInstanceTwoTriangleShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    EXPECT_THROW( m_proxy->createGeometry( m_fakeContext, m_stream ), std::runtime_error );
}

TEST_F( TestSceneProxy, coarseObjectInstanceAllShapesSamePrimitiveNotDecomposable )
{
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceTwoTriangleShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    const bool decomposable{ m_proxy->isDecomposable() };

    EXPECT_FALSE( decomposable );
    EXPECT_EQ( toOptixAabb( m_scene->bounds ), m_proxy->getBounds() );
}

TEST_F( TestSceneProxy, coarseObjectInstanceTriangleMeshShapePlyMeshShapeNotDecomposable )
{
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    MockMeshLoaderPtr meshLoader{ createMockMeshLoader() };
    MeshInfo          meshInfo{};
    EXPECT_CALL( *meshLoader, getMeshInfo() ).WillRepeatedly( Return( meshInfo ) );
    m_scene = singleInstanceTriangleMesShapePlyMeshShapeScene( meshLoader );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    const bool decomposable{ m_proxy->isDecomposable() };

    EXPECT_FALSE( decomposable );
    EXPECT_EQ( toOptixAabb( m_scene->bounds ), m_proxy->getBounds() );
}

TEST_F( TestSceneProxy, coarseObjectInstanceMixedMaterialTypesDecomposable )
{
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceTwoTriangleMixedMaterialTypesShapeScene( "triangles" );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    const bool decomposable{ m_proxy->isDecomposable() };

    EXPECT_TRUE( decomposable );
    EXPECT_EQ( toOptixAabb( m_scene->bounds ), m_proxy->getBounds() );
}

TEST_F( TestSceneProxy, coarseObjectInstanceMixedMaterialTextureTypesDecomposable )
{
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceThreeTriangleMixedTextureTypesShapeScene( "triangles" );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    const bool decomposable{ m_proxy->isDecomposable() };

    EXPECT_TRUE( decomposable );
    EXPECT_EQ( toOptixAabb( m_scene->bounds ), m_proxy->getBounds() );
}

TEST_F( TestSceneProxy, coarseObjectSamePrimitiveYieldsSingleGeometry )
{
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceTwoTriangleShapeScene();
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );
    GeometryCacheEntry triangles{};
    triangles.accelBuffer = 0xdeadbeefULL;
    triangles.traversable = m_fakeGeometryAS;
    triangles.primitive   = GeometryPrimitive::TRIANGLE;
    triangles.primitiveGroupEndIndices.push_back( 0 );
    triangles.primitiveGroupEndIndices.push_back( 1 );
    const std::string& name{ m_scene->objects.begin()->first };
    EXPECT_CALL( *m_geometryCache, getObject( m_fakeContext, m_stream, m_scene->objects[name], m_scene->objectShapes[name],
                                              GeometryPrimitive::TRIANGLE, MaterialFlags::NONE ) )
        .WillOnce( Return( triangles ) );

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };

    EXPECT_EQ( triangles.accelBuffer, geom.accelBuffer );
    EXPECT_EQ( triangles.primitive, geom.primitive );
    EXPECT_EQ( triangles.traversable, geom.instance.traversableHandle );
    ASSERT_EQ( 2U, geom.groups.size() );
    EXPECT_EQ( materialGroupFromPlasticMaterial( expectedMaterial(), 0U ), geom.groups[0] );
    EXPECT_EQ( materialGroupFromPlasticMaterial( expectedSecondMaterial(), 1U ), geom.groups[1] );
    EXPECT_EQ( triangles.devNormals, geom.devNormals );
    EXPECT_EQ( triangles.devUVs, geom.devUVs );
}

TEST_F( TestSceneProxy, createSceneInstancePrimitiveProxy )
{
    const std::string name{ "triangles" };
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceTwoTriangleMixedMaterialTypesShapeScene( name );
    const GeometryPrimitive         primitive{ GeometryPrimitive::TRIANGLE };
    const MaterialFlags             flags{ MaterialFlags::ALPHA_MAP | MaterialFlags::DIFFUSE_MAP };
    const ObjectInstanceDefinition& instance{ m_scene->objectInstances[0] };
    const ShapeDefinition&          shape{ m_scene->objectShapes[instance.name][0] };
    const pbrt::Bounds3f            bounds{ instance.transform( shape.transform( shape.bounds ) ) };
    expectProxyBoundsAdded( bounds, m_pageId );

    m_proxy = m_factory->sceneInstancePrimitive( m_scene, 0, primitive, flags );

    ASSERT_NE( nullptr, m_proxy );
    EXPECT_EQ( toOptixAabb( bounds ), m_proxy->getBounds() );
    EXPECT_EQ( m_pageId, m_proxy->getPageId() );
    const ProxyFactoryStatistics stats{ m_factory->getStatistics() };
    EXPECT_EQ( 0, stats.numSceneProxiesCreated );
    EXPECT_EQ( 0, stats.numShapeProxiesCreated );
    EXPECT_EQ( 0, stats.numInstanceProxiesCreated );
    EXPECT_EQ( 0, stats.numInstanceShapeProxiesCreated );
    EXPECT_EQ( 1, stats.numInstancePrimitiveProxiesCreated );
    EXPECT_EQ( 1, stats.numGeometryProxiesCreated );
}

TEST_F( TestSceneProxy, coarseObjectInstanceMixedMaterialsGeometry )
{
    const std::string name{ "triangles" };
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceTwoTriangleMixedMaterialTypesShapeScene( name );
    const GeometryPrimitive primitive{ GeometryPrimitive::TRIANGLE };
    const MaterialFlags     flags{ MaterialFlags::ALPHA_MAP | MaterialFlags::DIFFUSE_MAP };
    EXPECT_CALL( *m_geometryLoader, add( _ ) ).WillOnce( Return( m_pageId ) );
    m_proxy = m_factory->sceneInstancePrimitive( m_scene, 0, primitive, flags );
    GeometryCacheEntry triangles{};
    triangles.accelBuffer = 0xdeadbeefULL;
    triangles.traversable = m_fakeGeometryAS;
    triangles.primitive   = primitive;
    triangles.primitiveGroupEndIndices.push_back( 0 );
    EXPECT_CALL( *m_geometryCache,
                 getObject( m_fakeContext, m_stream, m_scene->objects[name], m_scene->objectShapes[name], primitive, flags ) )
        .WillOnce( Return( triangles ) );

    const GeometryInstance geom{ m_proxy->createGeometry( m_fakeContext, m_stream ) };

    EXPECT_EQ( triangles.accelBuffer, geom.accelBuffer );
    EXPECT_EQ( triangles.primitive, geom.primitive );
    EXPECT_EQ( triangles.traversable, geom.instance.traversableHandle );
    EXPECT_EQ( flags, geom.groups[0].material.flags );
    EXPECT_EQ( DIFFUSE_MAP_FILENAME, geom.groups[0].diffuseMapFileName );
    EXPECT_EQ( ALPHA_MAP_FILENAME, geom.groups[0].alphaMapFileName );
    EXPECT_EQ( triangles.devNormals, geom.devNormals );
    EXPECT_EQ( triangles.devUVs, geom.devUVs );
}

TEST_F( TestSceneProxy, coarseObjectInstanceSomeShapesDifferentPrimitiveDecomposable )
{
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceOneTriangleOneSphereShapeScene( "triangleSphere" );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );

    EXPECT_TRUE( m_proxy->isDecomposable() );
}

TEST_F( TestSceneProxy, coarseObjectInstanceMultiplePrimitivesDecomposed )
{
    const std::string objectName{ "triangleSphere" };
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceOneTriangleOneSphereShapeScene( objectName );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );
    const ObjectInstanceDefinition& instance{ m_scene->objectInstances[0] };
    const uint_t                    childId1{ 1111 };
    const ::ShapeDefinition&        shape1{ m_scene->objectShapes[objectName][0] };
    const OptixAabb          shape1Bounds{ toOptixAabb( instance.transform( shape1.transform( shape1.bounds ) ) ) };
    const uint_t             childId2{ 2222 };
    const ::ShapeDefinition& shape2{ m_scene->objectShapes[objectName][1] };
    const OptixAabb          shape2Bounds{ toOptixAabb( instance.transform( shape2.transform( shape2.bounds ) ) ) };
    EXPECT_CALL( *m_geometryLoader, add( shape1Bounds ) ).WillOnce( Return( childId1 ) );
    EXPECT_CALL( *m_geometryLoader, add( shape2Bounds ) ).WillOnce( Return( childId2 ) );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    ASSERT_EQ( 2U, parts.size() );
    const SceneProxyPtr& proxy1{ parts[0] };
    EXPECT_EQ( childId1, proxy1->getPageId() );
    EXPECT_EQ( shape1Bounds, proxy1->getBounds() );
    const SceneProxyPtr& proxy2{ parts[1] };
    EXPECT_EQ( childId2, proxy2->getPageId() );
    EXPECT_EQ( shape2Bounds, proxy2->getBounds() );
}

TEST_F( TestSceneProxy, coarseObjectInstanceMixedMaterialTypesDecomposed )
{
    const std::string objectName{ "triangleSphere" };
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceTwoTriangleMixedMaterialTypesShapeScene( objectName );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );
    const ObjectInstanceDefinition& instance{ m_scene->objectInstances[0] };
    const uint_t                    childId1{ 1111 };
    const ::ShapeDefinition&        shape1{ m_scene->objectShapes[objectName][0] };
    const OptixAabb          shape1Bounds{ toOptixAabb( instance.transform( shape1.transform( shape1.bounds ) ) ) };
    const uint_t             childId2{ 2222 };
    const ::ShapeDefinition& shape2{ m_scene->objectShapes[objectName][1] };
    const OptixAabb          shape2Bounds{ toOptixAabb( instance.transform( shape2.transform( shape2.bounds ) ) ) };
    EXPECT_CALL( *m_geometryLoader, add( shape1Bounds ) ).WillOnce( Return( childId1 ) );
    EXPECT_CALL( *m_geometryLoader, add( shape2Bounds ) ).WillOnce( Return( childId2 ) );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    ASSERT_EQ( 2U, parts.size() );
    const SceneProxyPtr& proxy1{ parts[0] };
    EXPECT_EQ( childId1, proxy1->getPageId() );
    EXPECT_EQ( shape1Bounds, proxy1->getBounds() );
    const SceneProxyPtr& proxy2{ parts[1] };
    EXPECT_EQ( childId2, proxy2->getPageId() );
    EXPECT_EQ( shape2Bounds, proxy2->getBounds() );
}

TEST_F( TestSceneProxy, coarseObjectInstanceMixedMaterialTextureTypesDecomposed )
{
    const std::string objectName{ "triangleSphere" };
    m_options.proxyGranularity = ProxyGranularity::COARSE;
    m_scene                    = singleInstanceThreeTriangleMixedTextureTypesShapeScene( objectName );
    expectProxyBoundsAdded( m_scene->bounds, m_pageId );
    m_proxy = m_factory->sceneInstance( m_scene, 0 );
    const ObjectInstanceDefinition& instance{ m_scene->objectInstances[0] };
    const ShapeList&                shapes{ m_scene->objectShapes[objectName] };
    const uint_t                    childId1{ 1111 };
    const ::ShapeDefinition&        shape1{ shapes[0] };
    const OptixAabb          shape1Bounds{ toOptixAabb( instance.transform( shape1.transform( shape1.bounds ) ) ) };
    const uint_t             childId2{ 2222 };
    const ::ShapeDefinition& shape2{ shapes[1] };
    const OptixAabb          shape2Bounds{ toOptixAabb( instance.transform( shape2.transform( shape2.bounds ) ) ) };
    const uint_t             childId3{ 3333 };
    const ShapeDefinition    shape3{ shapes[2] };
    const OptixAabb          shape3Bounds{ toOptixAabb( instance.transform( shape3.transform( shape3.bounds ) ) ) };
    EXPECT_CALL( *m_geometryLoader, add( shape1Bounds ) ).WillOnce( Return( childId1 ) );
    EXPECT_CALL( *m_geometryLoader, add( shape2Bounds ) ).WillOnce( Return( childId2 ) );
    EXPECT_CALL( *m_geometryLoader, add( shape3Bounds ) ).WillOnce( Return( childId3 ) );

    std::vector<SceneProxyPtr> parts{ m_proxy->decompose( m_factory ) };

    ASSERT_EQ( 3U, parts.size() );
    const SceneProxyPtr& proxy1{ parts[0] };
    EXPECT_EQ( childId1, proxy1->getPageId() );
    EXPECT_EQ( shape1Bounds, proxy1->getBounds() );
    const SceneProxyPtr& proxy2{ parts[1] };
    EXPECT_EQ( childId2, proxy2->getPageId() );
    EXPECT_EQ( shape2Bounds, proxy2->getBounds() );
    const SceneProxyPtr& proxy3{ parts[2] };
    EXPECT_EQ( childId3, proxy3->getPageId() );
    EXPECT_EQ( shape3Bounds, proxy3->getBounds() );
}
