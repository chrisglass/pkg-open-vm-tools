/*
 *  Author: bwilliams
 *  Created: April 6, 2012
 *
 *  Copyright (C) 2012-2016 VMware, Inc.  All rights reserved. -- VMware Confidential
 *
 *  This code was generated by the script "build/dev/codeGen/genCppXml". Please
 *  speak to Brian W. before modifying it by hand.
 *
 */

#ifndef ClassIdentifierXml_h_
#define ClassIdentifierXml_h_


#include "Doc/SchemaTypesDoc/CClassIdentifierDoc.h"

#include "Doc/DocXml/SchemaTypesXml/SchemaTypesXmlLink.h"
#include "Xml/XmlUtils/CXmlElement.h"

namespace Caf {

	/// Streams the ClassIdentifier class to/from XML
	namespace ClassIdentifierXml {

		/// Adds the ClassIdentifierDoc into the XML.
		void SCHEMATYPESXML_LINKAGE add(
			const SmartPtrCClassIdentifierDoc classIdentifierDoc,
			const SmartPtrCXmlElement thisXml);

		/// Parses the ClassIdentifierDoc from the XML.
		SmartPtrCClassIdentifierDoc SCHEMATYPESXML_LINKAGE parse(
			const SmartPtrCXmlElement thisXml);
	}
}

#endif
