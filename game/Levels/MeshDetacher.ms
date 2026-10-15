fn get_object name = execute ("$" + name)

max create mode
clearSelection()
select $Level
max modify mode
--modPanel.setCurrentObject $Level.modifiers[#Final_Smooth]
maxOps.CollapseNode $Level off

subobjectLevel = 0
convertTo $ PolyMeshObject
subobjectLevel = 4
modPanel.setCurrentObject $Level

for subobject = 1 to 1000 do
(
	namestring= "subobject" + subobject as string

	mymaterial=$.material[subobject]
	
	$.selectByMaterial subobject
	faces=polyop.getFaceSelection $
	
	if faces.count > 0 then 
	(
		created=polyop.detachfaces $ faces asNode:true name:namestring
		
		max create mode
		clearSelection()
		
		select (get_object namestring)
		
		addModifier $ (Smooth autosmooth:true threshold:50)
		
		
		
		--
		-- Okay, we wanna add a bunch of slice modifiers to chop up the scene 
		-- in a grid.  How do I get control of that?
		--
		
		
		subobjectLevel = 4
		$.material=mymaterial
		
		select $Level
		max modify mode
		)
		
	else exit
)

delete $Level

--select $subobject1
--$.material="Purple"

