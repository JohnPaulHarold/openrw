#include <render/GameRenderer.hpp>
#include <engine/GameWorld.hpp>
#include <engine/Animator.hpp>
#include <render/TextureAtlas.hpp>
#include <render/Model.hpp>

#include <objects/CharacterObject.hpp>
#include <objects/InstanceObject.hpp>
#include <objects/VehicleObject.hpp>
#include <ai/CharacterController.hpp>
#include <data/ObjectData.hpp>

#include <deque>
#include <cmath>
#include <glm/gtc/type_ptr.hpp>

const char *vertexShaderSource = "#version 130\n#extension GL_ARB_explicit_attrib_location : enable\n"
"layout(location = 0) in vec3 position;"
"layout(location = 1) in vec3 normal;"
"layout(location = 2) in vec4 colour;"
"layout(location = 3) in vec2 texCoords;"
"out vec3 Normal;"
"out vec2 TexCoords;"
"out vec4 Colour;"
"out vec4 EyeSpace;"
"uniform mat4 model;"
"uniform mat4 view;"
"uniform mat4 proj;"
"void main()"
"{"
"	Normal = normal;"
"	TexCoords = texCoords;"
"	Colour = colour;"
"	vec4 eyeSpace = view * model * vec4(position, 1.0);"
"	EyeSpace = proj * eyeSpace;"
"	gl_Position = proj * eyeSpace;"
"}";
const char *fragmentShaderSource = "#version 130\n"
"in vec3 Normal;"
"in vec2 TexCoords;"
"in vec4 Colour;"
"in vec4 EyeSpace;"
"uniform sampler2D texture;"
"uniform vec4 BaseColour;"
"uniform vec4 AmbientColour;"
"uniform vec4 DynamicColour;"
"uniform vec3 SunDirection;"
"uniform float FogStart;"
"uniform float FogEnd;"
"uniform float MaterialDiffuse;"
"uniform float MaterialAmbient;"
"void main()"
"{"
"   vec4 c = texture2D(texture, TexCoords);"
"	if(c.a < 0.1) discard;"
"	float fogZ = (gl_FragCoord.z / gl_FragCoord.w);"
"	float fogfac = clamp( (FogEnd-fogZ)/(FogEnd-FogStart), 0.0, 1.0 );"
//"	gl_FragColor = mix(AmbientColour, c, fogfac);"
"	gl_FragColor = mix(AmbientColour, BaseColour * (vec4(0.5) + Colour * 0.5) * (vec4(0.5) + DynamicColour * 0.5) * c, fogfac);"
"}";

const char *skydomeVertexShaderSource = "#version 130\n"
"in vec3 position;"
"uniform mat4 view;"
"uniform mat4 proj;"
"out vec3 Position;"
"uniform float Far;"
"void main() {"
"	Position = position;"
"	vec4 viewsp = proj * mat4(mat3(view)) * vec4(position, 1.0);"
"	viewsp.z = viewsp.w - 0.000001;"
"	gl_Position = viewsp;"
"}";
const char *skydomeFragmentShaderSource = "#version 130\n"
"in vec3 Position;"
"uniform vec4 TopColor;"
"uniform vec4 BottomColor;"
"void main() {"
"	gl_FragColor = mix(BottomColor, TopColor, clamp(Position.z, 0, 1));"
"}";
const size_t skydomeSegments = 8, skydomeRows = 10;

struct WaterVertex {
	static const AttributeList vertex_attributes() {
		return {
			{ATRS_Position, 2, sizeof(WaterVertex),  0ul}
		};
	}

	float x, y;
};

std::vector<WaterVertex> planeVerts = {
	{1.0f, 1.0f},
	{0.0f, 1.0f},
	{1.0f,-0.0f},
	{0.0f,-0.0f}
};

GeometryBuffer waterBuffer;
DrawBuffer waterDraw;

const char *waterVSSource = R"(
#version 130
#extension GL_ARB_explicit_attrib_location : enable
layout(location = 0) in vec2 position;
out vec2 TexCoords;
uniform float height;
uniform float size;
uniform mat4 MVP;
void main()
{
	TexCoords = position * 2.0;
	gl_Position = MVP * vec4(position * size, height, 1.0);
})";

const char *waterFSSource = R"(
#version 130
in vec3 Normal;
in vec2 TexCoords;
uniform sampler2D texture;
void main() {
	vec4 c = texture2D(texture, TexCoords);
	gl_FragColor = c;
})";


GLuint compileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		GLint len;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		GLchar *buffer = new GLchar[len];
		glGetShaderInfoLog(shader, len, NULL, buffer);

		std::cerr << "ERROR compiling shader: " << buffer << "\nSource: " << source << std::endl;
		delete[] buffer;
		exit(1);
	}

	return shader;
}

GameRenderer::GameRenderer(GameWorld* engine)
	: engine(engine), _renderAlpha(0.f)
{	
	GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
	GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
	worldProgram = glCreateProgram();
	glAttachShader(worldProgram, vertexShader);
	glAttachShader(worldProgram, fragmentShader);
	glLinkProgram(worldProgram);
	glUseProgram(worldProgram);
	
	uniModel = glGetUniformLocation(worldProgram, "model");
	uniView = glGetUniformLocation(worldProgram, "view");
	uniProj = glGetUniformLocation(worldProgram, "proj");
	uniCol = glGetUniformLocation(worldProgram, "BaseColour");
	uniAmbientCol = glGetUniformLocation(worldProgram, "AmbientColour");
	uniSunDirection = glGetUniformLocation(worldProgram, "SunDirection");
	uniDynamicCol = glGetUniformLocation(worldProgram, "DynamicColour");
	uniMatDiffuse = glGetUniformLocation(worldProgram, "MaterialDiffuse");
	uniMatAmbient = glGetUniformLocation(worldProgram, "MaterialAmbient");
	uniFogStart = glGetUniformLocation(worldProgram, "FogStart");
	uniFogEnd = glGetUniformLocation(worldProgram, "FogEnd");
	
	vertexShader = compileShader(GL_VERTEX_SHADER, skydomeVertexShaderSource);
	fragmentShader = compileShader(GL_FRAGMENT_SHADER, skydomeFragmentShaderSource);
	skyProgram = glCreateProgram();
	glAttachShader(skyProgram, vertexShader);
	glAttachShader(skyProgram, fragmentShader);
	glLinkProgram(skyProgram);
	glUseProgram(skyProgram);
	skyUniView = glGetUniformLocation(skyProgram, "view");
	skyUniProj = glGetUniformLocation(skyProgram, "proj");
	skyUniTop = glGetUniformLocation(skyProgram, "TopColor");
	skyUniBottom = glGetUniformLocation(skyProgram, "BottomColor");
	
	glGenVertexArrays( 1, &vao );

	// Upload water plane
	waterBuffer.uploadVertices(planeVerts);
	waterDraw.addGeometry(&waterBuffer);
	waterDraw.setFaceType(GL_TRIANGLE_STRIP);

	GLuint waterVS = compileShader(GL_VERTEX_SHADER, waterVSSource);
	GLuint waterFS = compileShader(GL_FRAGMENT_SHADER, waterFSSource);
	waterProgram = glCreateProgram();
	glAttachShader(waterProgram, waterVS);
	glAttachShader(waterProgram, waterFS);
	glLinkProgram(waterProgram);
	waterHeight = glGetUniformLocation(waterProgram, "height");
	waterTexture = glGetUniformLocation(waterProgram, "texture");
	waterSize = glGetUniformLocation(waterProgram, "size");
	waterMVP = glGetUniformLocation(waterProgram, "MVP");
	
	// And our skydome while we're at it.
	glGenBuffers(1, &skydomeVBO);
	glBindBuffer(GL_ARRAY_BUFFER, skydomeVBO);
	size_t segments = skydomeSegments, rows = skydomeRows;

    float R = 1.f/(float)(rows-1);
    float S = 1.f/(float)(segments-1);
    glm::vec3 skydomeBuff[rows * segments];
    for( size_t r = 0, i = 0; r < rows; ++r) {
        for( size_t s = 0; s < segments; ++s) {
            skydomeBuff[i++] = glm::vec3(
                        cos(2.f * M_PI * s * S) * cos(M_PI_2 * r * R),
                        sin(2.f * M_PI * s * S) * cos(M_PI_2 * r * R),
                        sin(M_PI_2 * r * R)
                        );
		}
	}
	glBufferData(GL_ARRAY_BUFFER, sizeof(skydomeBuff), skydomeBuff, GL_STATIC_DRAW);

    glGenBuffers(1, &skydomeIBO);
    GLushort skydomeIndBuff[rows*segments*6];
    for( size_t r = 0, i = 0; r < (rows-1); ++r ) {
        for( size_t s = 0; s < (segments-1); ++s ) {
            skydomeIndBuff[i++] = r * segments + s;
            skydomeIndBuff[i++] = r * segments + (s+1);
            skydomeIndBuff[i++] = (r+1) * segments + (s+1);
            skydomeIndBuff[i++] = r * segments + s;
            skydomeIndBuff[i++] = (r+1) * segments + (s+1);
            skydomeIndBuff[i++] = (r+1) * segments + s;
        }
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, skydomeIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(skydomeIndBuff), skydomeIndBuff, GL_STATIC_DRAW);

    glGenBuffers(1, &debugVBO);
    glGenTextures(1, &debugTex);
    glGenVertexArrays(1, &debugVAO);
}

float mix(uint8_t a, uint8_t b, float num)
{
	return a+(b-a)*num;
}

#define GL_PLS() \
{ auto errc = glGetError(); \
	if(errc != GL_NO_ERROR) std::cout << __LINE__ << ": " << errc << std::endl;\
}

void GameRenderer::renderWorld(float alpha)
{
	_renderAlpha = alpha;

	glBindVertexArray( vao );
	
    float tod = fmod(engine->gameTime, 24.f * 60.f);

	// Requires a float 0-24
    auto weather = engine->gameData.weatherLoader.getWeatherData(WeatherLoader::Sunny, tod / 60.f);

    glm::vec3 skyTop = weather.skyTopColor;
    glm::vec3 skyBottom = weather.skyBottomColor;
    glm::vec3 ambient = weather.ambientColor;
    glm::vec3 dynamic = weather.directLightColor;

	float theta = (tod/(60.f * 24.f) - 0.5f) * 2 * 3.14159265;
	glm::vec3 sunDirection{
		sin(theta),
		0.0,
		cos(theta),
	};
    sunDirection = glm::normalize(sunDirection);
    camera.frustum.far = weather.farClipping;

	glUseProgram(worldProgram);

    glUniform1f(uniFogStart, weather.fogStart);
	glUniform1f(uniFogEnd, camera.frustum.far);

	glUniform4f(uniAmbientCol, ambient.x, ambient.y, ambient.z, 1.f);
	glUniform4f(uniDynamicCol, dynamic.x, dynamic.y, dynamic.z, 1.f);
	glUniform3f(uniSunDirection, sunDirection.x, sunDirection.y, sunDirection.z);
	glUniform1f(uniMatDiffuse, 0.9f);
	glUniform1f(uniMatAmbient, 0.1f);
	
	glClearColor(skyBottom.r, skyBottom.g, skyBottom.b, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glm::mat4 proj = camera.frustum.projection();
	glm::mat4 view = camera.frustum.view;
	glUniformMatrix4fv(uniView, 1, GL_FALSE, glm::value_ptr(view));
	glUniformMatrix4fv(uniProj, 1, GL_FALSE, glm::value_ptr(proj));
	
	camera.frustum.update(camera.frustum.projection() * view);
	
	rendered = culled = 0;

	for(size_t i = 0; i < engine->pedestrians.size(); ++i) {
		CharacterObject* charac = engine->pedestrians[i];

        glm::mat4 matrixModel;
		
		matrixModel = glm::translate(matrixModel, charac->getPosition());
		matrixModel = matrixModel * glm::mat4_cast(charac->getRotation());

		if(!charac->model->model) continue;

		renderModel(charac->model->model, matrixModel, charac, charac->animator);
    }
	
	for(size_t i = 0; i < engine->objectInstances.size(); ++i) {
		InstanceObject& inst = *engine->objectInstances[i];
		
		if(inst.object->timeOn != inst.object->timeOff) {
			// Update rendering flags.
			if(engine->getHour() < inst.object->timeOn 
				&& engine->getHour() > inst.object->timeOff) {
				continue;
			}
		}

		if(!inst.model->model)
		{
			continue;
		}

		glm::mat4 matrixModel;
		if( inst.body ) {
			inst.body->getWorldTransform().getOpenGLMatrix(glm::value_ptr(matrixModel));
		}
		else {
			matrixModel = glm::translate(matrixModel, inst.position);
			matrixModel = glm::scale(matrixModel, inst.scale);
			matrixModel = matrixModel * glm::mat4_cast(inst.rotation);
		}

		float mindist = 100000.f;
		for (size_t g = 0; g < inst.model->model->geometries.size(); g++)
		{
			RW::BSGeometryBounds& bounds = inst.model->model->geometries[g]->geometryBounds;
			mindist = std::min(mindist, glm::length((glm::vec3(matrixModel[3])+bounds.center) - camera.worldPos) - bounds.radius);
		}

		if( inst.object->numClumps == 1 ) {
			if( mindist > inst.object->drawDistance[0] ) {
				// Check for LOD instances
				if ( inst.LODinstance ) {
					if( mindist > inst.LODinstance->object->drawDistance[0] ) {
						culled++;
						continue;
					}
					else if (inst.LODinstance->model->model) {
						renderModel(inst.LODinstance->model->model, matrixModel);
					}
				}
			}
			else if (! inst.object->LOD ) {
				renderModel(inst.model->model, matrixModel);
			}
		}
		else {
			if( mindist > inst.object->drawDistance[1] ) {
				culled++;
				continue;
			}
			else if( mindist > inst.object->drawDistance[0] ) {
				// Figure out which one is the LOD.
				auto RF = inst.model->model->frames[0];
				auto LODindex = RF->getChildren().size() - 2;
				auto f = RF->getChildren()[LODindex];
				renderFrame(inst.model->model, f, matrixModel * glm::inverse(f->getTransform()), nullptr);
			}
			else {
				// Draw the real object
				auto RF = inst.model->model->frames[0];
				auto LODindex = RF->getChildren().size() - 1;
				auto f = RF->getChildren()[LODindex];
				renderFrame(inst.model->model, f, matrixModel * glm::inverse(f->getTransform()), nullptr);
			}
		}
	}
	
	for(size_t v = 0; v < engine->vehicleInstances.size(); ++v) {
		VehicleObject* inst = engine->vehicleInstances[v];

		if(!inst->model)
		{
			std::cout << "model " <<  inst->vehicle->modelName << " not loaded (" << engine->gameData.models.size() << " models loaded)" << std::endl;
		}
		
		glm::mat4 matrixModel;
		matrixModel = glm::translate(matrixModel, inst->getPosition());
		matrixModel = matrixModel * glm::mat4_cast(inst->getRotation());

		renderModel(inst->model->model, matrixModel, inst);

		// Draw wheels n' stuff
		for( size_t w = 0; w < inst->info->wheels.size(); ++w) {
			auto woi = engine->objectTypes.find(inst->vehicle->wheelModelID);
			if(woi != engine->objectTypes.end()) {
				Model* wheelModel = engine->gameData.models["wheels"]->model;
				if( wheelModel ) {
					// Tell bullet to update the matrix for this wheel.
					inst->physVehicle->updateWheelTransform(w, false);
					glm::mat4 wheel_tf;
					inst->physVehicle->getWheelTransformWS(w).getOpenGLMatrix(glm::value_ptr(wheel_tf));
					wheel_tf = glm::scale(wheel_tf, glm::vec3(inst->vehicle->wheelScale));
					if(inst->physVehicle->getWheelInfo(w).m_chassisConnectionPointCS.x() < 0.f) {
						wheel_tf = glm::scale(wheel_tf, glm::vec3(-1.f, 1.f, 1.f));
					}
					renderWheel(wheelModel, wheel_tf, woi->second->modelName);
				}
				else {
					std::cout << "Wheel model " << woi->second->modelName << " not loaded" << std::endl;
				}
			}
		}
	}
	
	// Draw anything that got queued.
	for(auto it = transparentDrawQueue.begin();
		it != transparentDrawQueue.end();
		++it)
	{
		glUniformMatrix4fv(uniModel, 1, GL_FALSE, glm::value_ptr(it->matrix));
		glUniform4f(uniCol, 1.f, 1.f, 1.f, 1.f);
		
		renderSubgeometry(it->model, it->g, it->sg, it->matrix, it->object, false);
	}
	transparentDrawQueue.clear();

	// Draw the water.
	glBindVertexArray( waterDraw.getVAOName() );
	glUseProgram( waterProgram );

	// TODO: Add some kind of draw distance

	float blockLQSize = WATER_WORLD_SIZE/WATER_LQ_DATA_SIZE;
	float blockHQSize = WATER_WORLD_SIZE/WATER_HQ_DATA_SIZE;

	glm::vec2 waterOffset { -WATER_WORLD_SIZE/2.f, -WATER_WORLD_SIZE/2.f };
	glm::mat4 waterModel;
	glUniform1i(waterTexture, 0);
	auto waterTex = engine->gameData.textures["water_old"];
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, waterTex.texName);

	auto camposFlat = glm::vec2(camera.worldPos);

	// Draw High detail water
	glUniform1f(waterSize, blockHQSize);
	for( int x = 0; x < WATER_HQ_DATA_SIZE; x++ ) {
		for( int y = 0; y < WATER_HQ_DATA_SIZE; y++ ) {
			auto waterWS = waterOffset + glm::vec2(blockHQSize) * glm::vec2(x, y);
			auto cullWS = waterWS + (blockHQSize / 2.f);

			// Check that this is the right time to draw the HQ water
			if( glm::distance(camposFlat, cullWS) - blockHQSize >= WATER_HQ_DISTANCE ) continue;

			waterModel = glm::mat4();
			waterModel = glm::translate(waterModel, glm::vec3(waterWS, 0.f));
			int i = (x*WATER_HQ_DATA_SIZE) + y;
			int hI = engine->gameData.realWater[i];
			if( hI >= NO_WATER_INDEX ) continue;
			float h = engine->gameData.waterHeights[hI];

			glUniform1f(waterHeight, h);
			auto MVP = proj * view * waterModel;
			glUniformMatrix4fv(waterMVP, 1, GL_FALSE, glm::value_ptr(MVP));
			glDrawArrays(waterDraw.getFaceType(), 0, 4);
		}
	}

	glUniform1f(waterSize, blockLQSize);
	for( int x = 0; x < WATER_LQ_DATA_SIZE; x++ ) {
		for( int y = 0; y < WATER_LQ_DATA_SIZE; y++ ) {
			auto waterWS = waterOffset + glm::vec2(blockLQSize) * glm::vec2(x, y);
			auto cullWS = waterWS + (blockLQSize / 2.f);

			// Check that this is the right time to draw the LQ
			if( glm::distance(camposFlat, cullWS) - blockHQSize/4.f < WATER_HQ_DISTANCE ) continue;
			if( glm::distance(camposFlat, cullWS) - blockLQSize/2.f > camera.frustum.far ) continue;

			waterModel = glm::mat4();
			waterModel = glm::translate(waterModel, glm::vec3(waterWS, 0.f));
			int i = (x*WATER_LQ_DATA_SIZE) + y;
			int hI = engine->gameData.visibleWater[i];
			if( hI >= NO_WATER_INDEX ) continue;
			float h = engine->gameData.waterHeights[hI];

			glUniform1f(waterHeight, h);
			auto MVP = proj * view * waterModel;
			glUniformMatrix4fv(waterMVP, 1, GL_FALSE, glm::value_ptr(MVP));
			glDrawArrays(waterDraw.getFaceType(), 0, 4);
		}
	}

	glBindVertexArray( vao );
	
	glUseProgram(skyProgram);
	
	glBindBuffer(GL_ARRAY_BUFFER, skydomeVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, skydomeIBO);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);
	glUniformMatrix4fv(skyUniView, 1, GL_FALSE, glm::value_ptr(view));
	glUniformMatrix4fv(skyUniProj, 1, GL_FALSE, glm::value_ptr(proj));
	glUniform4f(skyUniTop, skyTop.r, skyTop.g, skyTop.b, 1.f);
	glUniform4f(skyUniBottom, skyBottom.r, skyBottom.g, skyBottom.b, 1.f);

	glDrawElements(GL_TRIANGLES, skydomeSegments * skydomeRows * 6, GL_UNSIGNED_SHORT, NULL);
	
	glUseProgram(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray( 0 );
}

void GameRenderer::renderWheel(Model* model, const glm::mat4 &matrix, const std::string& name)
{
	for (const ModelFrame* f : model->frames) 
	{
		const std::string& fname = f->getName();
		if( fname != name ) {
			continue;
		}

		auto firstLod = f->getChildren()[0];

		for( auto& g : firstLod->getGeometries() ) {
			RW::BSGeometryBounds& bounds = model->geometries[g]->geometryBounds;
			if(! camera.frustum.intersects(bounds.center + glm::vec3(matrix[3]), bounds.radius)) {
				culled++;
				continue;
			}

			renderGeometry(model, g, matrix);
		}
		break;
	}
}

void GameRenderer::renderGeometry(Model* model, size_t g, const glm::mat4& modelMatrix, GameObject* object)
{
    glUniformMatrix4fv(uniModel, 1, GL_FALSE, glm::value_ptr(modelMatrix));
    glUniform4f(uniCol, 1.f, 1.f, 1.f, 1.f);

	for(size_t sg = 0; sg < model->geometries[g]->subgeom.size(); ++sg)
	{
		if(! renderSubgeometry(model, g, sg, modelMatrix, object)) {
			// If rendering was rejected, queue for later.
			transparentDrawQueue.push_back(
				{model, g, sg, modelMatrix, object}
			);
		}
    }
}

bool GameRenderer::renderFrame(Model* m, ModelFrame* f, const glm::mat4& matrix, GameObject* object, bool queueTransparent)
{
	auto localmatrix = matrix;

	if(object && object->animator) {
		localmatrix *= object->animator->getFrameMatrix(f, _renderAlpha, object->isAnimationFixed());
	}
	else {
		localmatrix *= f->getTransform();
	}

	bool vis = object == nullptr || object->isFrameVisible(f);
	for(size_t g : f->getGeometries()) {
		if(!vis ) continue;

		RW::BSGeometryBounds& bounds = m->geometries[g]->geometryBounds;
		if(! camera.frustum.intersects(bounds.center + glm::vec3(matrix[3]), bounds.radius)) {
			continue;
		}

		if( (m->geometries[g]->flags & RW::BSGeometry::ModuleMaterialColor) != RW::BSGeometry::ModuleMaterialColor) {
			glUniform4f(uniCol, 1.f, 1.f, 1.f, 1.f);
		}

		renderGeometry(m,
						g, localmatrix,
						object);
	}
	
	for(ModelFrame* c : f->getChildren()) {
		renderFrame(m, c, localmatrix, object, queueTransparent);
	}
	return true;
}

bool GameRenderer::renderSubgeometry(Model* model, size_t g, size_t sg, const glm::mat4& matrix, GameObject* object, bool queueTransparent)
{
	glBindVertexArray(model->geometries[g]->dbuff.getVAOName());

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model->geometries[g]->EBO);

	auto& subgeom = model->geometries[g]->subgeom[sg];

	if (model->geometries[g]->materials.size() > subgeom.material) {
		Model::Material& mat = model->geometries[g]->materials[subgeom.material];

		if(mat.textures.size() > 0 ) {
			auto t = engine->gameData.textures.find(mat.textures[0].name);
			if(t != engine->gameData.textures.end()) {
				TextureInfo& tex = t->second;
				if(tex.transparent && queueTransparent) {
					return false;
				}
				glBindTexture(GL_TEXTURE_2D, tex.texName);
			}
		}

		if( (model->geometries[g]->flags & RW::BSGeometry::ModuleMaterialColor) == RW::BSGeometry::ModuleMaterialColor) {
			auto col = mat.colour;
			if( object && object->type() == GameObject::Vehicle ) {
				auto vehicle = static_cast<VehicleObject*>(object);
				if( (mat.flags&Model::MTF_PrimaryColour) != 0 ) {
					glUniform4f(uniCol, vehicle->colourPrimary.r, vehicle->colourPrimary.g, vehicle->colourPrimary.b, 1.f);
				}
				else if( (mat.flags&Model::MTF_SecondaryColour) != 0 ) {
					glUniform4f(uniCol, vehicle->colourSecondary.r, vehicle->colourSecondary.g, vehicle->colourSecondary.b, 1.f);
				}
				else {
					glUniform4f(uniCol, col.r/255.f, col.g/255.f, col.b/255.f, 1.f);
				}
			}
			else {
				glUniform4f(uniCol, col.r/255.f, col.g/255.f, col.b/255.f, 1.f);
			}
		}

		glUniform1f(uniMatDiffuse, mat.diffuseIntensity);
		glUniform1f(uniMatAmbient, mat.ambientIntensity);
	}

	rendered++;

	glDrawElements(model->geometries[g]->dbuff.getFaceType(),
								subgeom.numIndices, GL_UNSIGNED_INT, (void*)(sizeof(uint32_t) * subgeom.start));
	
	return true;
}

void GameRenderer::renderModel(Model* model, const glm::mat4& modelMatrix, GameObject* object, Animator *animator)
{
	renderFrame(model, model->frames[model->rootFrameIdx], modelMatrix, object);
}

void GameRenderer::renderPaths()
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, debugTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    static std::vector<glm::vec3> carlines;
    static std::vector<glm::vec3> pedlines;

    GLint posAttrib = glGetAttribLocation(worldProgram, "position");
    GLint uniModel = glGetUniformLocation(worldProgram, "model");

    glBindVertexArray( vao );

    for( size_t n = 0; n < engine->aigraph.nodes.size(); ++n ) {
        auto start = engine->aigraph.nodes[n];
		
		if( start->type == AIGraphNode::Pedestrian ) {
			pedlines.push_back(start->position);
			if( start->external ) {
				pedlines.push_back(start->position+glm::vec3(0.f, 0.f, 2.f));
			}
			else {
				pedlines.push_back(start->position+glm::vec3(0.f, 0.f, 1.f));
			}
		}	
		else {
			carlines.push_back(start->position-glm::vec3(start->size / 2.f, 0.f, 0.f));
			carlines.push_back(start->position+glm::vec3(start->size / 2.f, 0.f, 0.f));
		}

		for( size_t c = 0; c < start->connections.size(); ++c ) {
			auto end = start->connections[c];
			
			if( start->type == AIGraphNode::Pedestrian ) {	
				pedlines.push_back(start->position + glm::vec3(0.f, 0.f, 1.f));
				pedlines.push_back(end->position + glm::vec3(0.f, 0.f, 1.f));
			}
			else {
				carlines.push_back(start->position + glm::vec3(0.f, 0.f, 1.f));
				carlines.push_back(end->position + glm::vec3(0.f, 0.f, 1.f));
			}
		}
    }


	for(size_t i = 0; i < engine->pedestrians.size(); ++i) {
		CharacterObject* charac = engine->pedestrians[i];

		if(charac->controller) {
			carlines.push_back(charac->getPosition());
			carlines.push_back(charac->controller->getTargetPosition());
		}
	}

    glm::mat4 model;
    glUniformMatrix4fv(uniModel, 1, GL_FALSE, glm::value_ptr(model));
    glEnableVertexAttribArray(posAttrib);

    glBindBuffer(GL_ARRAY_BUFFER, debugVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * carlines.size(), &(carlines[0]), GL_STREAM_DRAW);
    glVertexAttribPointer(posAttrib, 3, GL_FLOAT, GL_FALSE, 0, 0);

    float img[] = {1.f, 0.f, 0.f};
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGB, 1, 1,
        0, GL_RGB, GL_FLOAT, img
    );

    glDrawArrays(GL_LINES, 0, carlines.size());

    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * pedlines.size(), &(pedlines[0]), GL_STREAM_DRAW);
    glVertexAttribPointer(posAttrib, 3, GL_FLOAT, GL_FALSE, 0, 0);

    float img2[] = {0.f, 1.f, 0.f};
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGB, 1, 1,
        0, GL_RGB, GL_FLOAT, img2
    );

    glDrawArrays(GL_LINES, 0, pedlines.size());

    pedlines.clear();
    carlines.clear();
    glBindVertexArray( 0 );
}