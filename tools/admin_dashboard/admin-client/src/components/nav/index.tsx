import React, { useState, useEffect } from "react";
import { useGo, useMenu } from "@refinedev/core";
import { useNavigate } from "react-router";

interface DashboardNavProps {
  isOpen: boolean;
}

export const DashboardNav: React.FC<DashboardNavProps> = ({ isOpen }) => {
  const { selectedKey, menuItems } = useMenu();
  const go = useGo();
  const navigate = useNavigate();

  const handleNavClick = (route?: string) => {
    if (route) {
      navigate(route);
    }
  };

  useEffect(() => {
	const handleKeyDown = (e: KeyboardEvent) => {
		const target = e.target as HTMLElement;
		if (
			target.tagName === "INPUT" ||
			target.tagName === "TEXTAREA" ||
			target.tagName === "SELECT" ||
			target.isContentEditable
		) {
			return;
		}
		if (e.key.match("[1-8]")) {
			console.log(`Key pressed: ${e.key}`);
			handleNavClick(menuItems[parseInt(e.key, 10) - 1].route);
		}
	}
	window.addEventListener("keydown", handleKeyDown);
	return () => {
		window.removeEventListener("keydown", handleKeyDown);
	};
  }, [menuItems]);

  return (
    <nav
      id="menu"
      className={`flex items-center flex-1 min-w-0
        gap-[clamp(14px,1.8vw,26px)] ml-[clamp(8px,2vw,26px)]
        max-[1080px]:fixed max-[1080px]:top-[68px]
        max-[1080px]:inset-x-0 max-[1080px]:flex-col
        max-[1080px]:items-stretch max-[1080px]:gap-0
        max-[1080px]:m-0 max-[1080px]:bg-[rgba(7,8,8,0.98)]
        max-[1080px]:border-b max-[1080px]:border-line
        max-[1080px]:pt-2 max-[1080px]:px-4 max-[1080px]:pb-4
        max-[1080px]:max-h-[calc(100vh-68px)]
        max-[1080px]:overflow-y-auto z-50
        ${isOpen ? "max-[1080px]:flex" : "max-[1080px]:hidden"}`}
    >
      {
		menuItems.map((item) => {
          const isSelected = item.key === selectedKey;
          return (
            <button
              key={item.key}
              className={`mi ${isSelected ? "active" : ""}`}
              data-tab={item.name}
              onClick={() => handleNavClick(item.route)}
            >
              {item.label}
            </button>
          );
        })
	  }
    </nav>
  );
};