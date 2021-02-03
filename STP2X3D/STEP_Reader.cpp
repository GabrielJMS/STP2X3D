#include "stdafx.h"
#include "STEP_Reader.h"
#include "STEP_Data.h"
#include "Component.h"
#include "IShape.h"

STEP_Reader::STEP_Reader(S2X_Option* opt)
	: m_opt(opt),
	m_stepData(nullptr)
{
	m_defaultFaceColor.SetValues(0.55f, 0.55f, 0.6f, 1.0f);	// Grey
	m_defaultWireColor.SetValues(1.0f, 1.0f, 1.0f, 1.0f);	// White
}

STEP_Reader::~STEP_Reader(void)
{
	Clear();
}

bool STEP_Reader::ReadSTEP(Model* model)
{
	IFSelect_ReturnStatus status;
	wstring filePath = m_opt->Input();

	try
	{
		// Read a STEP file
		STEPCAFControl_Reader cafReader;
		cafReader.SetNameMode(true);
		cafReader.SetColorMode(true);

		TCollection_AsciiString aFileName((const wchar_t*)filePath.c_str());
		status = cafReader.ReadFile(aFileName.ToCString());
		//status = cafReader.ReadFile(filePath.c_str());

		if (!CheckReturnStatus(status))
			return false;

		Handle(TDocStd_Document) doc = new TDocStd_Document("MDTV-XCAF");

		if (cafReader.Transfer(doc))
		{
			m_stepData = new STEP_Data(cafReader.Reader());
			m_shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
			m_colorTool = XCAFDoc_DocumentTool::ColorTool(doc->Main());
			
			// Update color option if there's no color
			UpdateColorOption();

			// Get free shapes
			TDF_LabelSequence labels_shapes;
			m_shapeTool->GetFreeShapes(labels_shapes);

			// It's usually 1 for assemblies
			int shapeSize = labels_shapes.Length();

			// Save shapes, structure, and colors to the model
			for (int i = 1; i <= shapeSize; ++i)
			{
				const TDF_Label& label_shape = labels_shapes.Value(i);
				const TopoDS_Shape& shape = m_shapeTool->GetShape(label_shape);

				Component* rootComp = new Component(shape);
				rootComp->SetUniqueName(GetName(label_shape));
				model->AddRootComponent(rootComp);

				AddSubComponents(rootComp, label_shape);
			}
		}
	}
	catch (Standard_NumericError& e)
	{
		model->Clear();
		UpdateColorOption();

		const char* errMsg = e.GetMessageString();
		cout << "\tStandard_NumericError(" << errMsg << ") occured." << endl;
		cout << "\tColor will not be supported." << endl;

		// Read a STEP file
		STEPControl_Reader reader;
		TCollection_AsciiString aFileName((const wchar_t*)filePath.c_str());
		status = reader.ReadFile(aFileName.ToCString());
		//status = reader.ReadFile(filePath.c_str());

		if (!CheckReturnStatus(status))
			return false;

		if (reader.TransferRoot())
		{
			const TopoDS_Shape& shape = reader.Shape();
			IShape* iShape = new IShape(shape);

			Component* rootComp = new Component(shape);
			rootComp->AddIShape(iShape);
			model->AddRootComponent(rootComp);
		}
	}
	catch (...)
	{
		// Unknown failure
		return CheckReturnStatus(IFSelect_RetFail);
	}

	model->Update();

	// Terminate if the root comp is empty
	if (model->IsEmpty())
		return CheckReturnStatus(IFSelect_RetVoid);

	return true;
}

void STEP_Reader::AddSubComponents(Component* comp, const TDF_Label& label)
{
	// Check label type
	if (m_shapeTool->IsAssembly(label)) // Assembly or sub-assembly component
	{
		comp->SetUniqueName(GetName(label)); // Set the component's name

		TDF_LabelSequence label_comps;
        m_shapeTool->GetComponents(label, label_comps);

		int compSize = label_comps.Length();

		for (int i = 1; i <= compSize; ++i)
		{
			const TDF_Label& label_comp = label_comps.Value(i);
			AddSubComponents(comp, label_comp); // Recursive call
		}
	}
	else if (m_shapeTool->IsReference(label)) // Reference (Part or Asssembly)
	{
		TDF_Label label_ref;
		m_shapeTool->GetReferredShape(label, label_ref);

		const TopoDS_Shape& refShape = m_shapeTool->GetShape(label_ref);
		const gp_Trsf& trsf = m_shapeTool->GetLocation(label).Transformation();

		if (IsEmpty(refShape))
			return;

		Component* subComp = new Component(refShape);	// New subcomp
		subComp->SetUniqueName(GetName(label_ref));		// Set the name
		subComp->SetTransformation(trsf);				// Set the transformation
		comp->AddSubComponent(subComp);					// Add the subcomp (This should be here!)

		// Skip if the subcomp is a copy
		if (IsCopy(subComp))
			return;

		AddSubComponents(subComp, label_ref); // Recursive call
	}
	else if (m_shapeTool->IsSimpleShape(label)) // Part component or just shape
	{
		const TopoDS_Shape& shape = m_shapeTool->GetShape(label);

		if (IsEmpty(shape))
			return;

		vector<TopoDS_Shape> subShapes;
		GetSubShapes(shape, subShapes);
		
		// Add each sub shape
		for (const auto& subShape : subShapes)
		{
			IShape* iShape = new IShape(subShape);
			comp->AddIShape(iShape);

			AddColors(iShape); // Add colors for each face OR edge
		}

		subShapes.clear();
	}
}

void STEP_Reader::GetSubShapes(const TopoDS_Shape& shape, vector<TopoDS_Shape>& subShapes) const
{
	TopAbs_ShapeEnum shapeType = shape.ShapeType();

	if (shapeType == TopAbs_COMPOUND)
	{
		// Free wires or edges
		if (OCCUtil::IsFreeEdge(shape))
		{
			if (m_opt->Sketch())
				subShapes.push_back(shape);
			
			return;
		}
		
		// Solid or suface models in STEP
		if (m_stepData->IsSolidModel(shape)
			|| m_stepData->IsSurfaceModel(shape))
		{
			subShapes.push_back(shape);
			return;
		}
		
		// Mapped_item in STEP (Need to check)
		if (m_stepData->IsMappedItem(shape))
			return;

		// Traverse the compound shape
		for (TopoDS_Iterator exp(shape); exp.More(); exp.Next())
		{
			const TopoDS_Shape& subShape = exp.Value();
			GetSubShapes(subShape, subShapes); // Recursive call
		}
	}
	else if (shapeType == TopAbs_COMPSOLID
			|| shapeType == TopAbs_SOLID
			|| shapeType == TopAbs_SHELL)
	{
		subShapes.push_back(shape);
		return;
	}
}

wstring STEP_Reader::GetName(const TDF_Label& label) const
{
	Handle(TDataStd_Name) nameData;
	TCollection_ExtendedString nameText;
	
	if (label.FindAttribute(TDataStd_Name::GetID(), nameData))
        nameText = nameData->Get();
	
    std::stringstream ss;
    ss << nameText << endl;

    std::string s = ss.str();
    //cout << s << endl;
	
    std::wstring name = StrTool::s2ws (s); //L"dd"; //nameText.ToWideString();

	// Remove extra linefeed
	name = StrTool::RemoveCharacter(name, L"\r");
	name = StrTool::RemoveCharacter(name, L"\n");

	// Ignore the names below
	if (name == L"COMPOUND"
		|| name == L"COMPSOLID"
		|| name == L"SOLID"
		|| name == L"SHELL"
		|| (name.find(L"=>[") == 0
			&& name.find(L"]") == name.length() - 1))
		name.clear();

	return name;
}

bool STEP_Reader::IsCopy(Component* comp)
{
	const TopoDS_Shape& shape = comp->GetShape();
	int shapeID = OCCUtil::GetID(shape);

	// Check if there's a component with the same id in the map
	if (m_idComponentMap.find(shapeID) == m_idComponentMap.end())
		m_idComponentMap.insert({ shapeID,comp });
	else
	{
		Component* originalComp = m_idComponentMap[shapeID];
		comp->SetOriginalComponent(originalComp);

		return true;
	}

	return false;
}

bool STEP_Reader::IsEmpty(const TopoDS_Shape& shape) const
{
	if (shape.IsNull()
		|| (m_opt->Sketch() && !OCCUtil::HasEdge(shape))	// If sketch is on, check edges
		|| (!m_opt->Sketch() && !OCCUtil::HasFace(shape))) // If sketch is off, check faces
		return true;

	return false;
}

void STEP_Reader::AddColors(IShape* iShape) const
{
	if (!m_opt->Color())
		return;

	const TopoDS_Shape& shape = iShape->GetShape();
	
	// OCCT 7.4.0 doesn't read transparency from STEP AP242 files
	if (iShape->IsFaceSet()) // Face set (Solids, Shells)
	{
		// Get solid or shell color
		Quantity_ColorRGBA solidColor;
		bool isSolidColored = false;
		
		if (m_colorTool->GetColor(shape, XCAFDoc_ColorSurf, solidColor))
			isSolidColored = true;

		TopExp_Explorer ExpFace;
		for (ExpFace.Init(shape, TopAbs_FACE); ExpFace.More(); ExpFace.Next())
		{
			const TopoDS_Shape& face = ExpFace.Current();
			
			// Get face color
			Quantity_ColorRGBA faceColor;
			bool isFaceColored = false;

			if (m_colorTool->GetColor(face, XCAFDoc_ColorSurf, faceColor))
				isFaceColored = true;

			// Priority: face color -> solid color -> default color
			if (isFaceColored)
				iShape->AddColor(face, faceColor);
			else if (isSolidColored)
				iShape->AddColor(face, solidColor);
			else
				iShape->AddColor(face, m_defaultFaceColor);
		}
	}
	else // Wire set (Wires, Edges)
	{
		// Free wires
		TopExp_Explorer ExpWire;
		for (ExpWire.Init(shape, TopAbs_WIRE); ExpWire.More(); ExpWire.Next())
		{
			const TopoDS_Shape& wire = ExpWire.Current();
				
			// Get wire color
			Quantity_ColorRGBA wireColor;
			bool isWireColored = false;

			if (m_colorTool->GetColor(wire, XCAFDoc_ColorCurv, wireColor))
				isWireColored = true;
			
			TopExp_Explorer ExpEdge;
			for (ExpEdge.Init(wire, TopAbs_EDGE); ExpEdge.More(); ExpEdge.Next())
			{
				const TopoDS_Shape& edge = ExpEdge.Current();
					
				// Get edge color
				Quantity_ColorRGBA edgeColor;
				bool isEdgeColored = false;

				if (m_colorTool->GetColor(edge, XCAFDoc_ColorCurv, edgeColor))
					isEdgeColored = true;
				
				// Priority: edge color -> wire color -> default color
				if (isEdgeColored)
					iShape->AddColor(edge, edgeColor);
				else if (isWireColored)
					iShape->AddColor(edge, wireColor);
				else
					iShape->AddColor(edge, m_defaultWireColor);
			}
		}

		// Free edges
		TopExp_Explorer ExpEdge;
		for (ExpEdge.Init(shape, TopAbs_EDGE, TopAbs_WIRE); ExpEdge.More(); ExpEdge.Next())
		{
			const TopoDS_Shape& edge = ExpEdge.Current();

			// Get edge color
			Quantity_ColorRGBA edgeColor;
			bool isEdgeColored = false;

			if (m_colorTool->GetColor(edge, XCAFDoc_ColorCurv, edgeColor))
				isEdgeColored = true;

			// Priority: edge color -> default color
			if (isEdgeColored)
				iShape->AddColor(edge, edgeColor);
			else
				iShape->AddColor(edge, m_defaultWireColor);
		}
	}
}

bool STEP_Reader::CheckReturnStatus(const IFSelect_ReturnStatus& status) const
{
	bool isDone = false;
	
	if (status == IFSelect_RetDone)
		isDone = true;
	else
	{
		switch (status)
		{
		case IFSelect_RetError:
			printf("Not a valid STEP file.\n");
			break;
		case IFSelect_RetFail:
			printf("Reading has failed.\n");
			break;
		case IFSelect_RetVoid:
			printf("Nothing to translate.\n");
			break;
		case IFSelect_RetStop:
			printf("Reading has stopped.\n");
			break;
		}
	}

	return isDone;
}

void STEP_Reader::UpdateColorOption() const
{
	if (!m_opt->Color())
		return;

	if (!m_colorTool) // If the default STEP reader has failed
		m_opt->SetColor(0);
	else
	{
		TDF_LabelSequence colorLabels;
		m_colorTool->GetColors(colorLabels);

		// Check the number of colors defined in the model
		int colorSize = colorLabels.Length();

		if (colorSize == 0)
			m_opt->SetColor(0);
	}
}

void STEP_Reader::Clear(void)
{
	m_idComponentMap.clear();
	delete m_stepData;
}
